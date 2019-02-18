#include <v8.h>
#include <uv.h>
#include <nan.h>

#ifndef _WIN32
#include <dlfcn.h>
#else
// nothing
#endif

#include <deque>
#include <map>
#include <mutex>

using namespace v8;
using namespace node;

#define JS_STR(...) Nan::New<v8::String>(__VA_ARGS__).ToLocalChecked()
#define JS_INT(val) Nan::New<v8::Integer>(val)
#define JS_NUM(val) Nan::New<v8::Number>(val)
#define JS_FLOAT(val) Nan::New<v8::Number>(val)
#define JS_BOOL(val) Nan::New<v8::Boolean>(val)

namespace workernative {

void Init(Handle<Object> exports);
void RunInThread(uv_async_t *handle);
void DeleteAsync(uv_handle_t *handle);
class WorkerNative;
class RequestContextImpl;
class RequestContext;

Local<Array> pointerToArray(void *ptr) {
  uintptr_t n = (uintptr_t)ptr;
  Local<Array> result = Nan::New<Array>(2);
  result->Set(0, JS_NUM((uint32_t)(n >> 32)));
  result->Set(1, JS_NUM((uint32_t)(n & 0xFFFFFFFF)));
  return result;
}

void *arrayToPointer(Local<Array> array) {
  uintptr_t n = ((uintptr_t)array->Get(0)->Uint32Value() << 32) | (uintptr_t)array->Get(1)->Uint32Value();
  return (void *)n;
}

std::map<std::string, uintptr_t> nativeRequires;
RequestContextImpl *topRequestContext = nullptr;
thread_local uv_loop_t *eventLoop = nullptr;
thread_local int requestKeys = 0;

class WorkerNative : public ObjectWrap {
public:
  static Handle<Object> Initialize();
// protected:
  static NAN_METHOD(New);
  static NAN_METHOD(FromArray);
  static NAN_METHOD(ToArray);
  static NAN_METHOD(Dlclose);
  static NAN_METHOD(GetEventLoop);
  static NAN_METHOD(SetEventLoop);
  static NAN_METHOD(RequireNative);
  static NAN_METHOD(SetNativeRequire);

  static NAN_METHOD(Request);
  static NAN_METHOD(Respond);
  static NAN_METHOD(PushResult);
  static NAN_METHOD(PopResult);
  static NAN_METHOD(QueueAsyncRequest);
  static NAN_METHOD(QueueAsyncResponse);

  static bool Dlclose(const char *soPath);
  
  WorkerNative(WorkerNative *ovmo = nullptr);
  ~WorkerNative();

// protected:
  RequestContextImpl *requestContext;
  WorkerNative *oldWorkerNative;
};

class RequestContextImpl {
public:
  RequestContextImpl(uv_loop_t *loop);
  ~RequestContextImpl();

// protected:
  std::string result;
  uv_sem_t lockRequestSem;
  // uv_sem_t lockResponseSem;
  uv_sem_t requestSem;
  uv_async_t parentAsync;
  std::map<int, Nan::Persistent<Function>> parentAsyncFns;
  std::deque<std::pair<int, std::string>> parentAsyncQueue;
  std::deque<std::string> parentResultQueue;
  Nan::Persistent<Function> parentSyncHandler;
  std::deque<std::string> parentSyncQueue;
  std::mutex parentAsyncMutex;
};

class RequestContext : public ObjectWrap {
public:
  RequestContext(RequestContextImpl *requestContext = nullptr);
  ~RequestContext();

// protected:
  static Handle<Object> Initialize();
// protected:
  static NAN_METHOD(New);
  /* static NAN_METHOD(FromArray);
  static NAN_METHOD(ToArray); */
  static NAN_METHOD(PushResult);
  static NAN_METHOD(PopResult);
  static NAN_METHOD(SetSyncHandler);
  static NAN_METHOD(PushSyncRequest);
  static NAN_METHOD(GetTopRequestContext);
  static NAN_METHOD(SetTopRequestContext);

// protected:
  RequestContextImpl *requestContext;
};

Handle<Object> WorkerNative::Initialize() {
  Nan::EscapableHandleScope scope;

  // constructor
  Local<FunctionTemplate> ctor = Nan::New<FunctionTemplate>(New);
  ctor->InstanceTemplate()->SetInternalFieldCount(1);
  ctor->SetClassName(JS_STR("WorkerNative"));

  // prototype
  Local<ObjectTemplate> proto = ctor->PrototypeTemplate();
  Nan::SetMethod(proto, "toArray", ToArray);
  Nan::SetMethod(proto, "request", Request);
  Nan::SetMethod(proto, "respond", Respond);
  Nan::SetMethod(proto, "pushResult", PushResult);
  Nan::SetMethod(proto, "popResult", PopResult);
  Nan::SetMethod(proto, "queueAsyncRequest", QueueAsyncRequest);
  Nan::SetMethod(proto, "queueAsyncResponse", QueueAsyncResponse);

  Local<Function> ctorFn = ctor->GetFunction();
  ctorFn->Set(JS_STR("fromArray"), Nan::New<Function>(FromArray));
  ctorFn->Set(JS_STR("dlclose"), Nan::New<Function>(Dlclose));
  ctorFn->Set(JS_STR("getEventLoop"), Nan::New<Function>(GetEventLoop));
  ctorFn->Set(JS_STR("setEventLoop"), Nan::New<Function>(SetEventLoop));
  ctorFn->Set(JS_STR("requireNative"), Nan::New<Function>(RequireNative));
  ctorFn->Set(JS_STR("setNativeRequire"), Nan::New<Function>(SetNativeRequire));

  uintptr_t initFunctionAddress = (uintptr_t)workernative::Init;
  Local<Array> initFunctionAddressArray = Nan::New<Array>(2);
  initFunctionAddressArray->Set(0, Nan::New<Integer>((uint32_t)(initFunctionAddress >> 32)));
  initFunctionAddressArray->Set(1, Nan::New<Integer>((uint32_t)(initFunctionAddress & 0xFFFFFFFF)));
  ctorFn->Set(JS_STR("initFunctionAddress"), initFunctionAddressArray);

  return scope.Escape(ctorFn);
}

NAN_METHOD(WorkerNative::New) {
  Local<Object> vmOneObj = Local<Object>::Cast(info.This());

  WorkerNative *oldWorkerNative;
  if (info[0]->IsArray()) {
    Local<Array> array = Local<Array>::Cast(info[0]);
    uint32_t a = array->Get(0)->Uint32Value();
    uint32_t b = array->Get(1)->Uint32Value();
    uintptr_t c = ((uintptr_t)a << 32) | (uintptr_t)b;
    oldWorkerNative = reinterpret_cast<WorkerNative *>(c);
  } else {
    oldWorkerNative = nullptr;
  }

  WorkerNative *vmOne = new WorkerNative(oldWorkerNative);
  vmOne->Wrap(vmOneObj);

  info.GetReturnValue().Set(vmOneObj);
}

NAN_METHOD(WorkerNative::ToArray) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());

  Local<Array> array = Nan::New<Array>(2);
  array->Set(0, Nan::New<Integer>((uint32_t)((uintptr_t)vmOne >> 32)));
  array->Set(1, Nan::New<Integer>((uint32_t)((uintptr_t)vmOne & 0xFFFFFFFF)));

  info.GetReturnValue().Set(array);
}

NAN_METHOD(WorkerNative::FromArray) {
  Local<Array> array = Local<Array>::Cast(info[0]);

  Local<Function> vmOneConstructor = Local<Function>::Cast(info.This());
  Local<Value> argv[] = {
    array,
  };
  Local<Value> vmOneObj = vmOneConstructor->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), sizeof(argv)/sizeof(argv[0]), argv).ToLocalChecked();

  info.GetReturnValue().Set(vmOneObj);
}

bool WorkerNative::Dlclose(const char *soPath) {
#ifndef _WIN32
  void *handle = dlopen(soPath, RTLD_LAZY);

  if (handle) {
    while (dlclose(handle) == 0) {}
    
    return true;
  } else {
    return false;
  }
#else
  WCHAR soPath_w[32768];
  MultiByteToWideChar(CP_UTF8, 0, soPath, -1, soPath_w, sizeof(soPath_w)/sizeof(soPath_w[0]));
  
  HMODULE handle = LoadLibraryExW(soPath_w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (handle != NULL) {
    while (FreeLibrary(handle)) {}
    
    return true;
  } else {
    return false;
  }
#endif
}

NAN_METHOD(WorkerNative::Dlclose) {
  if (info[0]->IsString()) {
    Local<String> soPathString = Local<String>::Cast(info[0]);
    String::Utf8Value soPathUtf8Value(soPathString);
    const char *soPath = *soPathUtf8Value;
    
    if (Dlclose(soPath)) {
      // nothing
    } else {
      Nan::ThrowError("WorkerNative::Dlclose: failed to open handle to close");
    }
  } else {
    Nan::ThrowError("WorkerNative::Dlclose: invalid arguments");
  }
}

NAN_METHOD(WorkerNative::GetEventLoop) {
  if (eventLoop) {
    info.GetReturnValue().Set(pointerToArray(eventLoop));
  } else {
    info.GetReturnValue().Set(Nan::Null());
  }
}

NAN_METHOD(WorkerNative::SetEventLoop) {
  if (info[0]->IsArray()) {
    eventLoop = (uv_loop_t *)arrayToPointer(Local<Array>::Cast(info[0]));
  } else {
    Nan::ThrowError("SetEventLoop: invalid arguments");
  }
}

NAN_METHOD(WorkerNative::RequireNative) {
  Local<String> requireNameValue = info[0]->ToString();
  String::Utf8Value requireNameUtf8(requireNameValue);
  std::string requireName(*requireNameUtf8, requireNameUtf8.length());

  auto iter = nativeRequires.find(requireName);
  if (iter != nativeRequires.end()) {
    uintptr_t address = iter->second;
    void (*Init)(Handle<Object> exports) = (void (*)(Handle<Object>))address;

    Local<Object> exportsObj = Nan::New<Object>();
    Init(exportsObj);
    return info.GetReturnValue().Set(exportsObj);
  } else {
    return Nan::ThrowError("Native module not found");
  }
}

NAN_METHOD(WorkerNative::SetNativeRequire) {
  if (info[0]->IsString() && info[1]->IsArray()) {
    Local<String> requireNameValue = info[0]->ToString();
    String::Utf8Value requireNameUtf8(requireNameValue);
    std::string requireName(*requireNameUtf8, requireNameUtf8.length());

    Local<Array> requireAddressValue = Local<Array>::Cast(info[1]);
    uintptr_t requireAddress = ((uint64_t)requireAddressValue->Get(0)->Uint32Value() << 32) | ((uint64_t)requireAddressValue->Get(1)->Uint32Value() & 0xFFFFFFFF);

    if (requireAddress) {
      nativeRequires[requireName] = requireAddress;
    } else {
      Nan::ThrowError("init function address cannot be null");
    }
  } else {
    Nan::ThrowError("invalid arguments");
  }
}

WorkerNative::WorkerNative(WorkerNative *ovmo) : requestContext(nullptr), oldWorkerNative(nullptr) {
  if (!ovmo) {
    requestContext = new RequestContextImpl(eventLoop);
  } else {
    /* Local<Context> localContext = Isolate::GetCurrent()->GetCurrentContext();

    localContext->AllowCodeGenerationFromStrings(true);
    // ContextEmbedderIndex::kAllowWasmCodeGeneration = 34
    localContext->SetEmbedderData(34, Nan::New<Boolean>(true)); */

    requestContext = ovmo->requestContext;
    oldWorkerNative = ovmo;
  }
}

WorkerNative::~WorkerNative() {
  if (!oldWorkerNative) {
    delete requestContext;
  }
}

NAN_METHOD(WorkerNative::Request) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  RequestContextImpl *requestContext = vmOne->requestContext;
  uv_sem_wait(&requestContext->requestSem);
}

NAN_METHOD(WorkerNative::Respond) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  RequestContextImpl *requestContext = vmOne->requestContext;
  uv_sem_post(&requestContext->requestSem);
}

NAN_METHOD(WorkerNative::PushResult) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  RequestContextImpl *requestContext = vmOne->requestContext;

  std::string result;
  if (info[0]->IsString()) {
    Local<String> stringValue = Local<String>::Cast(info[0]);
    String::Utf8Value utf8Value(stringValue);
    result = std::string(*utf8Value, utf8Value.length());
  }
  
  {
    std::lock_guard<std::mutex> lock(requestContext->parentAsyncMutex);

    requestContext->parentResultQueue.push_back(std::move(result));
  }
  
  uv_sem_post(&requestContext->lockRequestSem);
  // uv_sem_wait(&requestContext->lockResponseSem);

  requestContext->result.clear();
}

NAN_METHOD(WorkerNative::PopResult) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  RequestContextImpl *requestContext = vmOne->requestContext;

  uv_sem_wait(&requestContext->lockRequestSem);

  std::string result;
  {
    std::lock_guard<std::mutex> lock(requestContext->parentAsyncMutex);

    result = std::move(requestContext->parentResultQueue.front());
    requestContext->parentResultQueue.pop_front();
  }
  
  Local<String> resultValue = JS_STR(result);
  // uv_sem_post(&requestContext->lockResponseSem);

  info.GetReturnValue().Set(resultValue);
}

NAN_METHOD(WorkerNative::QueueAsyncRequest) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  RequestContextImpl *requestContext = vmOne->requestContext;
  Local<Function> localFn = Local<Function>::Cast(info[0]);

  int requestKey = ++requestKeys;
  {
    std::lock_guard<std::mutex> lock(requestContext->parentAsyncMutex);

    requestContext->parentAsyncFns.emplace(requestKey, localFn);
  }

  info.GetReturnValue().Set(JS_INT(requestKey));
}

NAN_METHOD(WorkerNative::QueueAsyncResponse) {
  if (info[0]->IsNumber() && info[1]->IsString()) {
    WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
    RequestContextImpl *requestContext = vmOne->requestContext;
    int requestKey = info[0]->Int32Value();
    String::Utf8Value utf8Value(info[1]);

    {
      std::lock_guard<std::mutex> lock(requestContext->parentAsyncMutex);

      requestContext->parentAsyncQueue.emplace_back(requestKey, std::string(*utf8Value, utf8Value.length()));
    }

    uv_async_send(&requestContext->parentAsync);
  } else {
    Nan::ThrowError("WorkerNative::QueueAsyncResponse: invalid arguments");
  }
}

RequestContextImpl::RequestContextImpl(uv_loop_t *loop) {
  uv_sem_init(&lockRequestSem, 0);
  // uv_sem_init(&lockResponseSem, 0);
  uv_sem_init(&requestSem, 0);

  uv_async_init(loop, &parentAsync, RunInThread);
  parentAsync.data = this;
}

RequestContextImpl::~RequestContextImpl() {
  uv_sem_destroy(&lockRequestSem);
  // uv_sem_destroy(&lockResponseSem);
  uv_sem_destroy(&requestSem);
  
  uv_close((uv_handle_t *)(&parentAsync), DeleteAsync); // XXX clean up deletion here
}

Handle<Object> RequestContext::Initialize() {
  Nan::EscapableHandleScope scope;

  // constructor
  Local<FunctionTemplate> ctor = Nan::New<FunctionTemplate>(New);
  ctor->InstanceTemplate()->SetInternalFieldCount(1);
  ctor->SetClassName(JS_STR("RequestContext"));

  // prototype
  Local<ObjectTemplate> proto = ctor->PrototypeTemplate();
  // Nan::SetMethod(proto, "toArray", ToArray);
  Nan::SetMethod(proto, "pushResult", PushResult);
  Nan::SetMethod(proto, "popResult", PopResult);
  Nan::SetMethod(proto, "setSyncHandler", SetSyncHandler);
  Nan::SetMethod(proto, "pushSyncRequest", PushSyncRequest);

  Local<Function> ctorFn = ctor->GetFunction();
  // ctorFn->Set(JS_STR("fromArray"), Nan::New<Function>(FromArray));
  ctorFn->Set(JS_STR("getTopRequestContext"), Nan::New<Function>(GetTopRequestContext));
  ctorFn->Set(JS_STR("setTopRequestContext"), Nan::New<Function>(SetTopRequestContext));

  return scope.Escape(ctorFn);
}

NAN_METHOD(RequestContext::New) {
  Local<Object> requestContextObj = Local<Object>::Cast(info.This());

  RequestContextImpl *oldRequestContext;
  if (info[0]->IsArray()) {
    Local<Array> array = Local<Array>::Cast(info[0]);
    uint32_t a = array->Get(0)->Uint32Value();
    uint32_t b = array->Get(1)->Uint32Value();
    uintptr_t c = ((uintptr_t)a << 32) | (uintptr_t)b;
    oldRequestContext = reinterpret_cast<RequestContextImpl *>(c);
  } else {
    oldRequestContext = nullptr;
  }

  RequestContext *requestContext = new RequestContext(oldRequestContext);
  requestContext->Wrap(requestContextObj);

  info.GetReturnValue().Set(requestContextObj);
}

RequestContext::RequestContext(RequestContextImpl *rc) {
  if (rc) {
    requestContext = rc;
  } else {
    requestContext = new RequestContextImpl(eventLoop);
  }
}

RequestContext::~RequestContext() {}

NAN_METHOD(RequestContext::PushResult) {
  RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(info.This());
  RequestContextImpl *requestContextImpl = requestContext->requestContext;
  
  std::string result;
  if (info[0]->IsString()) {
    Local<String> stringValue = Local<String>::Cast(info[0]);
    String::Utf8Value utf8Value(stringValue);
    result = std::string(*utf8Value, utf8Value.length());
  }
  
  {
    std::lock_guard<std::mutex> lock(requestContextImpl->parentAsyncMutex);

    requestContextImpl->parentResultQueue.push_back(std::move(result));
  }
  
  uv_sem_post(&requestContextImpl->lockRequestSem);
  // uv_sem_wait(&requestContextImpl->lockResponseSem);

  requestContextImpl->result.clear();
}

NAN_METHOD(RequestContext::PopResult) {
  RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(info.This());
  RequestContextImpl *requestContextImpl = requestContext->requestContext;

  uv_sem_wait(&requestContextImpl->lockRequestSem);

  std::string result;
  {
    std::lock_guard<std::mutex> lock(requestContextImpl->parentAsyncMutex);

    result = std::move(requestContextImpl->parentResultQueue.front());
    requestContextImpl->parentResultQueue.pop_front();
  }
  
  Local<String> resultValue = JS_STR(result);
  // uv_sem_post(&requestContextImpl->lockResponseSem);

  info.GetReturnValue().Set(resultValue);
}

NAN_METHOD(RequestContext::SetSyncHandler) {
  if (info[0]->IsFunction()) {
    RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(info.This());
    Local<Function> localFn = Local<Function>::Cast(info[0]);
    RequestContextImpl *requestContextImpl = requestContext->requestContext;
    
    requestContextImpl->parentSyncHandler.Reset(localFn);
  } else {
    Nan::ThrowError("RequestContext::SetSyncHandler: invalid arguments");
  }
}

NAN_METHOD(RequestContext::PushSyncRequest) {
  if (info[0]->IsString()) {
    RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(info.This());
    String::Utf8Value utf8Value(info[0]);
    RequestContextImpl *requestContextImpl = requestContext->requestContext;
    
    {
      std::lock_guard<std::mutex> lock(requestContextImpl->parentAsyncMutex);

      requestContextImpl->parentSyncQueue.emplace_back(*utf8Value, utf8Value.length());
    }
    
    uv_async_send(&requestContextImpl->parentAsync);
  } else {
    Nan::ThrowError("RequestContext::PushSyncRequest: invalid arguments");
  }
}

/* NAN_METHOD(RequestContext::ToArray) {
  RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(info.This());

  Local<Array> array = Nan::New<Array>(2);
  array->Set(0, Nan::New<Integer>((uint32_t)((uintptr_t)requestContext >> 32)));
  array->Set(1, Nan::New<Integer>((uint32_t)((uintptr_t)requestContext & 0xFFFFFFFF)));

  info.GetReturnValue().Set(array);
}

NAN_METHOD(RequestContext::FromArray) {
  Local<Array> array = Local<Array>::Cast(info[0]);

  Local<Function> requestContextConstructor = Local<Function>::Cast(info.This());
  Local<Value> argv[] = {
    array,
  };
  Local<Value> requestContextObj = requestContextConstructor->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), sizeof(argv)/sizeof(argv[0]), argv).ToLocalChecked();

  info.GetReturnValue().Set(requestContextObj);
} */

NAN_METHOD(RequestContext::GetTopRequestContext) {
  if (topRequestContext) {
    Local<Function> requestContextConstructor = Local<Function>::Cast(info.This());
    Local<Array> array = pointerToArray(topRequestContext);
    Local<Value> argv[] = {
      array,
    };
    Local<Value> requestContextObj = requestContextConstructor->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), sizeof(argv)/sizeof(argv[0]), argv).ToLocalChecked();

    info.GetReturnValue().Set(requestContextObj);
  } else {
    info.GetReturnValue().Set(Nan::Null());
  }
}

NAN_METHOD(RequestContext::SetTopRequestContext) {
  if (info[0]->IsObject()) {
    RequestContext *requestContext = ObjectWrap::Unwrap<RequestContext>(Local<Object>::Cast(info[0]));
    topRequestContext = requestContext->requestContext;
  } else {
    Nan::ThrowError("RequestContext::SetTopRequestContext: invalid arguments");
  }
}

void RunInThread(uv_async_t *handle) {
  Nan::HandleScope scope;

  RequestContextImpl *requestContext = (RequestContextImpl *)(((uv_async_t *)handle)->data);

  std::deque<std::pair<int, std::string>> localParentAsyncQueue;
  std::vector<Local<Function>> localParentAsyncFns;
  Local<Function> localParentSyncHandlerFn;
  std::deque<std::string> localParentSyncQueue;
  {
    std::lock_guard<std::mutex> lock(requestContext->parentAsyncMutex);

    localParentAsyncQueue = std::move(requestContext->parentAsyncQueue);
    requestContext->parentAsyncQueue.clear();

    localParentAsyncFns.reserve(localParentAsyncQueue.size());
    for (size_t i = 0; i < localParentAsyncQueue.size(); i++) {
      const int &requestKey = localParentAsyncQueue[i].first;
      Nan::Persistent<Function> &fn = requestContext->parentAsyncFns[requestKey];
      localParentAsyncFns.push_back(Nan::New(fn));
      fn.Reset();
      requestContext->parentAsyncFns.erase(requestKey);
    }

    if (!requestContext->parentSyncHandler.IsEmpty()) {
      localParentSyncHandlerFn = Nan::New(requestContext->parentSyncHandler);
      
      localParentSyncQueue = std::move(requestContext->parentSyncQueue);
      requestContext->parentSyncQueue.clear();
    }
  }

  for (size_t i = 0; i < localParentAsyncQueue.size(); i++) {
    // Nan::HandleScope scope;
    
    Local<Function> &localFn = localParentAsyncFns[i];
    const std::string &requestResult = localParentAsyncQueue[i].second;

    Local<Object> asyncObj = Nan::New<Object>();
    AsyncResource asyncResource(Isolate::GetCurrent(), asyncObj, "RequestContextImpl::RunInThread Async");

    Local<Value> argv[] = {
      JS_STR(requestResult),
    };
    asyncResource.MakeCallback(localFn, sizeof(argv)/sizeof(argv[0]), argv);
  }

  for (size_t i = 0; i < localParentSyncQueue.size(); i++) {
    const std::string &requestString = localParentSyncQueue[i];

    Local<Object> asyncObj = Nan::New<Object>();
    AsyncResource asyncResource(Isolate::GetCurrent(), asyncObj, "RequestContextImpl::RunInThread Sync");

    Local<Value> argv[] = {
      JS_STR(requestString),
    };
    asyncResource.MakeCallback(localParentSyncHandlerFn, sizeof(argv)/sizeof(argv[0]), argv);
  }
}

void DeleteAsync(uv_handle_t *handle) {
  uv_async_t *async = (uv_async_t *)handle;
  delete async;
}

void Init(Handle<Object> exports) {
  exports->Set(JS_STR("WorkerNative"), WorkerNative::Initialize());
  exports->Set(JS_STR("RequestContext"), RequestContext::Initialize());
}

void RootInit(Handle<Object> exports) {
  Init(exports);
}

}

#ifndef LUMIN
NODE_MODULE(NODE_GYP_MODULE_NAME, workernative::RootInit)
#else
extern "C" {
  void node_register_module_worker_native(Local<Object> exports, Local<Value> module, Local<Context> context) {
    workernative::RootInit(exports);
  }
}
#endif
