/**
 * lzma-native - Node.js bindings for liblzma
 * Copyright (C) 2014-2015 Hauke Henningsen <sqrt@entless.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 **/

#include "liblzma-node.hpp"
#include <node_buffer.h>
#include <cstring>
#include <cstdlib>

namespace lzma {
#ifdef LZMA_ASYNC_AVAILABLE
const bool LZMAStream::asyncCodeAvailable = true;
#else
const bool LZMAStream::asyncCodeAvailable = false;
#endif

namespace {
	extern "C" void worker(void* opaque) {
		LZMAStream* self = static_cast<LZMAStream*>(opaque);
		
		self->doLZMACodeFromAsync();
	}
	
	extern "C" void invoke_buffer_handlers_async(uv_async_t* async
#if UV_VERSION_MAJOR < 1
	, int status
#endif
	) {
		LZMAStream* strm = static_cast<LZMAStream*>(async->data);
		strm->invokeBufferHandlersFromAsync();
	}

	extern "C" void async_close(uv_handle_t* handle) {
#ifdef LZMA_ASYNC_AVAILABLE
		delete reinterpret_cast<uv_async_t*>(handle);
#endif
	}
}

Nan::Persistent<Function> LZMAStream::constructor;

LZMAStream::LZMAStream()
	: hasRunningThread(false), hasPendingCallbacks(false), hasRunningCallbacks(false),
	isNearDeath(false), bufsize(8192), shouldFinish(false),
	shouldInvokeChunkCallbacks(false), lastCodeResult(LZMA_OK) 
{
	std::memset(&_, 0, sizeof(lzma_stream));

#ifdef LZMA_ASYNC_AVAILABLE
	uv_mutex_init(&mutex);
	uv_cond_init(&lifespanCond);
	uv_cond_init(&inputDataCond);
	
	outputDataAsync = new uv_async_t;
	uv_async_init(uv_default_loop(), outputDataAsync, invoke_buffer_handlers_async);
	outputDataAsync->data = static_cast<void*>(this);
#endif
}

void LZMAStream::resetUnderlying() {
	lzma_end(&_);
	
	std::memset(&_, 0, sizeof(lzma_stream));
	lastCodeResult = LZMA_OK;
}

LZMAStream::~LZMAStream() {
#ifdef LZMA_ASYNC_AVAILABLE
	{
		LZMA_ASYNC_LOCK(this);
		
		isNearDeath = true;
		uv_cond_broadcast(&inputDataCond);
		
		while (hasRunningThread || hasRunningCallbacks)
			uv_cond_wait(&lifespanCond, &mutex);
	}
#endif
	
	// no locking necessary from now on, we are the only active thread
	
	resetUnderlying();
	
#ifdef LZMA_ASYNC_AVAILABLE
	uv_mutex_destroy(&mutex);
	uv_cond_destroy(&lifespanCond);
	uv_cond_destroy(&inputDataCond);
	uv_close(reinterpret_cast<uv_handle_t*>(outputDataAsync), async_close);
#endif
}

#define LZMA_FETCH_SELF() \
	LZMAStream* self = NULL; \
	if (!info.This().IsEmpty() && info.This()->InternalFieldCount() > 0) { \
		self = Nan::ObjectWrap::Unwrap<LZMAStream>(info.This()); \
	} \
	if (!self) { \
		_failMissingSelf(info); \
		return; \
	}

NAN_METHOD(LZMAStream::Code) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	std::vector<uint8_t> inputData;
	
	Local<Object> bufarg = Local<Object>::Cast(info[0]);
	if (bufarg.IsEmpty() || bufarg->IsUndefined() || bufarg->IsNull()) {
		self->shouldFinish = true;
	} else {
		if (!readBufferFromObj(bufarg, inputData)) 
			info.GetReturnValue().SetUndefined();
		
		if (inputData.empty())
			self->shouldFinish = true;
	}
	
	self->inbufs.push(LZMA_NATIVE_MOVE(inputData));
	
	bool hadRunningThread = self->hasRunningThread;
	bool async = info[1]->BooleanValue() || hadRunningThread;
	self->hasRunningThread = async;
	
	if (async) {
#ifdef LZMA_ASYNC_AVAILABLE
		if (!hadRunningThread) {
			uv_thread_t worker_id;
			uv_thread_create(&worker_id, worker, static_cast<void*>(self));
		}
		
		uv_cond_broadcast(&self->inputDataCond);
#else
		std::abort();
#endif
	} else {
		self->doLZMACode(false);
	}
	
	info.GetReturnValue().SetUndefined();
}

void LZMAStream::invokeBufferHandlersFromAsync() {
	invokeBufferHandlers(false, false);
}

void LZMAStream::invokeBufferHandlers(bool async, bool hasLock) {
#ifdef LZMA_ASYNC_AVAILABLE
	uv_mutex_guard lock(mutex, !hasLock);
#define POSSIBLY_LOCK_MX    do { if (!hasLock) lock.lock(); } while(0)
#define POSSIBLY_UNLOCK_MX  do { if (!hasLock) lock.unlock(); } while(0)
#else
#define POSSIBLY_LOCK_MX
#define POSSIBLY_UNLOCK_MX
#endif

	if (!hasLock && !hasPendingCallbacks)
		return;
	
	if (async) {
#ifdef LZMA_ASYNC_AVAILABLE
		hasPendingCallbacks = true;
		
		// this calls invokeBufferHandler(false) from the main loop thread
		uv_async_send(outputDataAsync);
		return;
#else
		std::abort();
#endif
	}
	
	hasRunningCallbacks = true;
	hasPendingCallbacks = false;
	
	struct _ScopeGuard {
		_ScopeGuard(LZMAStream* self_) : self(self_) {}
		~_ScopeGuard() {
			self->hasRunningCallbacks = false;

#ifdef LZMA_ASYNC_AVAILABLE
			uv_cond_broadcast(&self->lifespanCond);
#endif
		}
		
		LZMAStream* self;
	};
	_ScopeGuard guard(this);
	
	Nan::HandleScope scope;
	
	Local<Function> bufferHandler = Local<Function>::Cast(EmptyToUndefined(Nan::Get(handle(), NewString("bufferHandler"))));
	std::vector<uint8_t> outbuf;
	
#define CALL_BUFFER_HANDLER_WITH_ARGV \
	POSSIBLY_UNLOCK_MX; \
	bufferHandler->Call(handle(), 3, argv); \
	POSSIBLY_LOCK_MX;
	
	while (outbufs.size() > 0) {
		outbuf = LZMA_NATIVE_MOVE(outbufs.front());
		outbufs.pop();
		
		Local<Value> argv[3] = {
			Nan::CopyBuffer(reinterpret_cast<const char*>(outbuf.data()), outbuf.size()).ToLocalChecked(),
			Nan::Undefined(), Nan::Undefined()
		};
		CALL_BUFFER_HANDLER_WITH_ARGV
	}
	
	if (lastCodeResult != LZMA_OK) {
		Local<Value> errorArg = Local<Value>(Nan::Null());
		
		if (lastCodeResult != LZMA_STREAM_END)
			errorArg = lzmaRetError(lastCodeResult);
		
		resetUnderlying(); // resets lastCodeResult!
		
		Local<Value> argv[3] = { Nan::Null(), Nan::Undefined(), errorArg };
		CALL_BUFFER_HANDLER_WITH_ARGV
	}
	
	if (shouldInvokeChunkCallbacks) {
		shouldInvokeChunkCallbacks = false;
		
		Local<Value> argv[3] = { Nan::Undefined(), Nan::New<Boolean>(true), Nan::Undefined() };
		CALL_BUFFER_HANDLER_WITH_ARGV
	}
}

void LZMAStream::doLZMACodeFromAsync() {
	LZMA_ASYNC_LOCK(this);
	
	struct _ScopeGuard {
		_ScopeGuard(LZMAStream* self_) : self(self_) {}
		~_ScopeGuard() {
			self->hasRunningThread = false;

#ifdef LZMA_ASYNC_AVAILABLE
			uv_cond_broadcast(&self->lifespanCond);
#endif
		}
		
		LZMAStream* self;
	};
	_ScopeGuard guard(this);
	
	doLZMACode(true);
}

void LZMAStream::doLZMACode(bool async) {
	bool invokedBufferHandlers = false;
	
	std::vector<uint8_t> outbuf(bufsize), inbuf;
	_.next_out = outbuf.data();
	_.avail_out = outbuf.size();
	_.avail_in = 0;

	lzma_action action = LZMA_RUN;
	
	shouldInvokeChunkCallbacks = false;
	bool hasConsumedChunks = false;
	
	// _.internal is set to NULL when lzma_end() is called via resetUnderlying()
	while (_.internal) {
		if (_.avail_in == 0 && _.avail_out != 0) { // more input neccessary?
			if (inbufs.empty()) { // more input available?
				if (async) {
#ifdef LZMA_ASYNC_AVAILABLE
					if (hasConsumedChunks) {
						shouldInvokeChunkCallbacks = true;
						
						invokeBufferHandlers(async, true);
						invokedBufferHandlers = true;
					}
					
					// wait until more data is available
					while (inbufs.empty() && !shouldFinish && !isNearDeath)
						uv_cond_wait(&inputDataCond, &mutex);
#else
					std::abort();
#endif
				}
				
				if (action == LZMA_FINISH || isNearDeath)
					break;
				
				if (shouldFinish)
					action = LZMA_FINISH;
				
				if (!shouldFinish && !async) {
					shouldInvokeChunkCallbacks = true;
					invokedBufferHandlers = false;
					break;
				}
			}
			
			while (_.avail_in == 0 && !inbufs.empty()) {
				inbuf = LZMA_NATIVE_MOVE(inbufs.front());
				inbufs.pop();
			
				_.next_in = inbuf.data();
				_.avail_in = inbuf.size();
				hasConsumedChunks = true;
			}
		}
			
		_.next_out = outbuf.data();
		_.avail_out = outbuf.size();
		
		invokedBufferHandlers = false;
		lastCodeResult = lzma_code(&_, action);
		
		if (lastCodeResult != LZMA_OK && lastCodeResult != LZMA_STREAM_END)
			break;
		
		if (_.avail_out == 0 || _.avail_in == 0 || lastCodeResult == LZMA_STREAM_END) {
			size_t outsz = outbuf.size() - _.avail_out;
			
			if (outsz > 0) {
#ifndef LZMA_NO_CXX11_RVALUE_REFERENCES // C++11
				outbufs.emplace(outbuf.data(), outbuf.data() + outsz);
#else
				outbufs.push(std::vector<uint8_t>(outbuf.data(), outbuf.data() + outsz));
#endif
			}
			
			// save status, since invokeBufferHandlers() may reset
			lzma_ret oldLCR = lastCodeResult;
			
			if (lastCodeResult == LZMA_STREAM_END)
				shouldInvokeChunkCallbacks = true;
			
			invokeBufferHandlers(async, true);
			invokedBufferHandlers = true;
			
			if (oldLCR == LZMA_STREAM_END)
				break;
		}
	}
	
	if (!invokedBufferHandlers)
		invokeBufferHandlers(async, true);
}

void LZMAStream::Init(Local<Object> exports) {
	Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
	tpl->SetClassName(NewString("LZMAStream"));
	tpl->InstanceTemplate()->SetInternalFieldCount(1);
	
	tpl->PrototypeTemplate()->Set(NewString("code"),           Nan::New<FunctionTemplate>(Code)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("memusage"),       Nan::New<FunctionTemplate>(Memusage)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("getCheck"),       Nan::New<FunctionTemplate>(GetCheck)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("memlimitGet"),    Nan::New<FunctionTemplate>(MemlimitGet)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("memlimitSet"),    Nan::New<FunctionTemplate>(MemlimitSet)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("totalIn"),        Nan::New<FunctionTemplate>(TotalIn)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("totalOut"),       Nan::New<FunctionTemplate>(TotalOut)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("rawEncoder_"),    Nan::New<FunctionTemplate>(RawEncoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("rawDecoder_"),    Nan::New<FunctionTemplate>(RawDecoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("filtersUpdate"),  Nan::New<FunctionTemplate>(FiltersUpdate)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("easyEncoder_"),   Nan::New<FunctionTemplate>(EasyEncoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("streamEncoder_"), Nan::New<FunctionTemplate>(StreamEncoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("aloneEncoder"),   Nan::New<FunctionTemplate>(AloneEncoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("streamDecoder_"), Nan::New<FunctionTemplate>(StreamDecoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("autoDecoder_"),   Nan::New<FunctionTemplate>(AutoDecoder)->GetFunction());
	tpl->PrototypeTemplate()->Set(NewString("aloneDecoder_"),  Nan::New<FunctionTemplate>(AloneDecoder)->GetFunction());
	constructor.Reset(tpl->GetFunction());
	exports->Set(NewString("Stream"), Nan::New<Function>(constructor));
}

NAN_METHOD(LZMAStream::New) {
	if (info.IsConstructCall()) {
		(new LZMAStream())->Wrap(info.This());
		info.GetReturnValue().Set(info.This());
	} else {
		Local<Value> argv[0] = {};
		info.GetReturnValue().Set(Nan::New<Function>(constructor)->NewInstance(0, argv));
	}
}

void LZMAStream::_failMissingSelf(const Nan::FunctionCallbackInfo<Value>& info) {
	Nan::ThrowTypeError("LZMAStream methods need to be called on an LZMAStream object");
	info.GetReturnValue().SetUndefined();
}

NAN_METHOD(LZMAStream::Memusage) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	info.GetReturnValue().Set(Uint64ToNumber0Null(lzma_memusage(&self->_)));
}

NAN_METHOD(LZMAStream::GetCheck) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	info.GetReturnValue().Set(Nan::New<Number>(lzma_get_check(&self->_)));
}

NAN_METHOD(LZMAStream::TotalIn) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	info.GetReturnValue().Set(Nan::New<Number>(self->_.total_in));
}

NAN_METHOD(LZMAStream::TotalOut) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	info.GetReturnValue().Set(Nan::New<Number>(self->_.total_out));
}

NAN_METHOD(LZMAStream::MemlimitGet) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	info.GetReturnValue().Set(Uint64ToNumber0Null(lzma_memlimit_get(&self->_)));
}

NAN_METHOD(LZMAStream::MemlimitSet) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	Local<Number> arg = Local<Number>::Cast(info[0]);
	if (info[0]->IsUndefined() || arg.IsEmpty()) {
		Nan::ThrowTypeError("memlimitSet() needs an number argument");
		info.GetReturnValue().SetUndefined();
	}
	
	info.GetReturnValue().Set(lzmaRet(lzma_memlimit_set(&self->_, NumberToUint64ClampNullMax(arg))));
}

NAN_METHOD(LZMAStream::RawEncoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	const FilterArray filters(Local<Array>::Cast(info[0]));
	
	info.GetReturnValue().Set(lzmaRet(lzma_raw_encoder(&self->_, filters.array())));
}

NAN_METHOD(LZMAStream::RawDecoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	const FilterArray filters(Local<Array>::Cast(info[0]));
	
	info.GetReturnValue().Set(lzmaRet(lzma_raw_decoder(&self->_, filters.array())));
}

NAN_METHOD(LZMAStream::FiltersUpdate) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	const FilterArray filters(Local<Array>::Cast(info[0]));
	
	info.GetReturnValue().Set(lzmaRet(lzma_filters_update(&self->_, filters.array())));
}

NAN_METHOD(LZMAStream::EasyEncoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	Local<Integer> preset = Local<Integer>::Cast(info[0]);
	Local<Integer> check = Local<Integer>::Cast(info[1]);
	
	info.GetReturnValue().Set(lzmaRet(lzma_easy_encoder(&self->_, preset->Value(), (lzma_check) check->Value())));
}

NAN_METHOD(LZMAStream::StreamEncoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	const FilterArray filters(Local<Array>::Cast(info[0]));
	Local<Integer> check = Local<Integer>::Cast(info[1]);
	
	info.GetReturnValue().Set(lzmaRet(lzma_stream_encoder(&self->_, filters.array(), (lzma_check) check->Value())));
}

NAN_METHOD(LZMAStream::AloneEncoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	Local<Object> opt = Local<Object>::Cast(info[0]);
	lzma_options_lzma o = parseOptionsLZMA(opt);
	
	info.GetReturnValue().Set(lzmaRet(lzma_alone_encoder(&self->_, &o)));
}

NAN_METHOD(LZMAStream::StreamDecoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	uint64_t memlimit = NumberToUint64ClampNullMax(info[0]);
	Local<Integer> flags = Local<Integer>::Cast(info[1]);
	
	info.GetReturnValue().Set(lzmaRet(lzma_stream_decoder(&self->_, memlimit, flags->Value())));
}

NAN_METHOD(LZMAStream::AutoDecoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	uint64_t memlimit = NumberToUint64ClampNullMax(info[0]);
	Local<Integer> flags = Local<Integer>::Cast(info[1]);
	
	info.GetReturnValue().Set(lzmaRet(lzma_auto_decoder(&self->_, memlimit, flags->Value())));
}

NAN_METHOD(LZMAStream::AloneDecoder) {
	LZMA_FETCH_SELF();
	LZMA_ASYNC_LOCK(self);
	
	uint64_t memlimit = NumberToUint64ClampNullMax(info[0]);
	
	info.GetReturnValue().Set(lzmaRet(lzma_alone_decoder(&self->_, memlimit)));
}

}
