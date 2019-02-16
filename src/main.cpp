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
void RunInParentThread(uv_async_t *handle);
void DeleteAsync(uv_handle_t *handle);
class WorkerNative;

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

thread_local uv_loop_t *eventLoop = nullptr;
std::map<uv_async_t *, WorkerNative *> asyncToWorkerParentMap;
std::mutex asyncToWorkerParentMutex;
std::map<std::string, uintptr_t> nativeRequires;

class WorkerNative : public ObjectWrap {
public:
  static Handle<Object> Initialize();
// protected:
  static NAN_METHOD(New);
  // static NAN_METHOD(GetGlobal);
  static NAN_METHOD(FromArray);
  static NAN_METHOD(ToArray);
  static NAN_METHOD(Dlclose);
  static NAN_METHOD(RequireNative);
  static NAN_METHOD(SetNativeRequire);
  static NAN_METHOD(Request);
  static NAN_METHOD(Respond);
  // static NAN_METHOD(PushGlobal);
  static NAN_METHOD(PushResult);
  static NAN_METHOD(PopResult);
  static NAN_METHOD(QueueAsyncRequest);
  static NAN_METHOD(QueueAsyncResponse);

  static bool Dlclose(const char *soPath);
  
  WorkerNative(WorkerNative *ovmo = nullptr);
  ~WorkerNative();

// protected:
  std::string result;
  uv_sem_t *lockRequestSem;
  uv_sem_t *lockResponseSem;
  uv_sem_t *requestSem;
  uv_async_t *parentAsync;
  std::map<int, Nan::Persistent<Function>> *parentAsyncFns;
  std::deque<std::pair<int, std::string>> *parentAsyncQueue;
  std::mutex *parentAsyncMutex;
  WorkerNative *oldWorkerNative;
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
  // Nan::SetMethod(proto, "getGlobal", GetGlobal);
  Nan::SetMethod(proto, "request", Request);
  Nan::SetMethod(proto, "respond", Respond);
  // Nan::SetMethod(proto, "pushGlobal", PushGlobal);
  Nan::SetMethod(proto, "pushResult", PushResult);
  Nan::SetMethod(proto, "popResult", PopResult);
  Nan::SetMethod(proto, "queueAsyncRequest", QueueAsyncRequest);
  Nan::SetMethod(proto, "queueAsyncResponse", QueueAsyncResponse);

  Local<Function> ctorFn = ctor->GetFunction();
  ctorFn->Set(JS_STR("fromArray"), Nan::New<Function>(FromArray));
  ctorFn->Set(JS_STR("dlclose"), Nan::New<Function>(Dlclose));
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

  WorkerNative *vmOne = oldWorkerNative ? new WorkerNative(oldWorkerNative) : new WorkerNative();
  vmOne->Wrap(vmOneObj);

  info.GetReturnValue().Set(vmOneObj);
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

NAN_METHOD(WorkerNative::ToArray) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());

  Local<Array> array = Nan::New<Array>(2);
  array->Set(0, Nan::New<Integer>((uint32_t)((uintptr_t)vmOne >> 32)));
  array->Set(1, Nan::New<Integer>((uint32_t)((uintptr_t)vmOne & 0xFFFFFFFF)));

  info.GetReturnValue().Set(array);
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

WorkerNative::WorkerNative(WorkerNative *ovmo) {
  if (!ovmo) {
    lockRequestSem = new uv_sem_t();
    uv_sem_init(lockRequestSem, 0);

    lockResponseSem = new uv_sem_t();
    uv_sem_init(lockResponseSem, 0);

    requestSem = new uv_sem_t();
    uv_sem_init(requestSem, 0);

    parentAsync = new uv_async_t();
    uv_loop_t *loop = eventLoop ? eventLoop : uv_default_loop();
    uv_async_init(loop, parentAsync, RunInParentThread);
    {
      std::lock_guard<std::mutex> lock(asyncToWorkerParentMutex);

      asyncToWorkerParentMap[parentAsync] = this;
    }

    parentAsyncFns = new std::map<int, Nan::Persistent<Function>>();
    parentAsyncQueue = new std::deque<std::pair<int, std::string>>();
    parentAsyncMutex = new std::mutex();
  } else {
    Local<Context> localContext = Isolate::GetCurrent()->GetCurrentContext();

    localContext->AllowCodeGenerationFromStrings(true);
    // ContextEmbedderIndex::kAllowWasmCodeGeneration = 34
    localContext->SetEmbedderData(34, Nan::New<Boolean>(true));

    lockRequestSem = ovmo->lockRequestSem;
    lockResponseSem = ovmo->lockResponseSem;
    requestSem = ovmo->requestSem;

    parentAsync = ovmo->parentAsync;
    parentAsyncFns = ovmo->parentAsyncFns;
    parentAsyncQueue = ovmo->parentAsyncQueue;
    parentAsyncMutex = ovmo->parentAsyncMutex;

    oldWorkerNative = ovmo;
  }
}

WorkerNative::~WorkerNative() {
  if (!oldWorkerNative) {
    uv_sem_destroy(lockRequestSem);
    delete lockRequestSem;

    uv_sem_destroy(lockResponseSem);
    delete lockResponseSem;

    uv_sem_destroy(requestSem);
    delete requestSem;

    uv_close((uv_handle_t *)parentAsync, DeleteAsync);
    {
      std::lock_guard<std::mutex> lock(asyncToWorkerParentMutex);

      asyncToWorkerParentMap.erase(parentAsync);
    }

    delete parentAsyncFns;
    delete parentAsyncQueue;
    delete parentAsyncMutex;
  }
}

/* NAN_METHOD(WorkerNative::PushGlobal) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  vmOne->oldWorkerNative->result.Reset(Isolate::GetCurrent()->GetCurrentContext()->Global());

  uv_sem_post(vmOne->lockRequestSem);
  uv_sem_wait(vmOne->lockResponseSem);

  vmOne->oldWorkerNative->result.Reset();
} */

NAN_METHOD(WorkerNative::PushResult) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  
  if (info[0]->IsString()) {
    Local<String> stringValue = Local<String>::Cast(info[0]);
    String::Utf8Value utf8Value(stringValue);
    vmOne->oldWorkerNative->result = std::string(*utf8Value, utf8Value.length());
  }
  
  uv_sem_post(vmOne->lockRequestSem);
  uv_sem_wait(vmOne->lockResponseSem);

  vmOne->oldWorkerNative->result.clear();
}

NAN_METHOD(WorkerNative::PopResult) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());

  uv_sem_wait(vmOne->lockRequestSem);
  Local<String> result = JS_STR(vmOne->result);
  uv_sem_post(vmOne->lockResponseSem);

  info.GetReturnValue().Set(result);
}

NAN_METHOD(WorkerNative::QueueAsyncRequest) {
  if (info[0]->IsFunction()) {
    WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
    Local<Function> localFn = Local<Function>::Cast(info[0]);

    int requestKey = rand();
    {
      std::lock_guard<std::mutex> lock(*(vmOne->parentAsyncMutex));

      vmOne->parentAsyncFns->emplace(requestKey, localFn);
    }

    info.GetReturnValue().Set(JS_INT(requestKey));
  } else {
    Nan::ThrowError("WorkerNative::QueueAsyncRequest: invalid arguments");
  }
}

NAN_METHOD(WorkerNative::QueueAsyncResponse) {
  if (info[0]->IsNumber() && info[1]->IsString()) {
    WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
    int requestKey = info[0]->Int32Value();
    String::Utf8Value utf8Value(info[1]);

    {
      std::lock_guard<std::mutex> lock(*(vmOne->parentAsyncMutex));

      vmOne->parentAsyncQueue->emplace_back(requestKey, std::string(*utf8Value, utf8Value.length()));
    }

    uv_async_send(vmOne->parentAsync);
  } else {
    Nan::ThrowError("WorkerNative::QueueAsyncResponse: invalid arguments");
  }
}

NAN_METHOD(nop) {}
void RunInParentThread(uv_async_t *handle) {
  Nan::HandleScope scope;

  WorkerNative *vmOne;
  {
    std::lock_guard<std::mutex> lock(asyncToWorkerParentMutex);

    vmOne = asyncToWorkerParentMap[(uv_async_t *)handle];
  }

  std::deque<std::pair<int, std::string>> localParentAsyncQueue;
  std::vector<Local<Function>> localParentAsyncFns;
  {
    std::lock_guard<std::mutex> lock(*(vmOne->parentAsyncMutex));

    localParentAsyncQueue = std::move(*(vmOne->parentAsyncQueue));
    vmOne->parentAsyncQueue->clear();

    localParentAsyncFns.reserve(localParentAsyncQueue.size());
    for (size_t i = 0; i < localParentAsyncQueue.size(); i++) {
      const int &requestKey = localParentAsyncQueue[i].first;
      Nan::Persistent<Function> &fn = (*(vmOne->parentAsyncFns))[requestKey];
      localParentAsyncFns.push_back(Nan::New(fn));
      vmOne->parentAsyncFns->erase(requestKey);
    }
  }

  for (size_t i = 0; i < localParentAsyncQueue.size(); i++) {
    Nan::HandleScope scope;
    
    Local<Function> &localFn = localParentAsyncFns[i];
    const std::string &requestResult = localParentAsyncQueue[i].second;

    Local<Object> asyncObj = Nan::New<Object>();
    AsyncResource asyncResource(Isolate::GetCurrent(), asyncObj, "WorkerNative::RunInParentThread");

    Local<Value> argv[] = {
      JS_STR(requestResult),
    };
    asyncResource.MakeCallback(localFn, sizeof(argv)/sizeof(argv[0]), argv);
  }
}

void DeleteAsync(uv_handle_t *handle) {
  uv_async_t *async = (uv_async_t *)handle;
  delete async;
}

/* NAN_METHOD(WorkerNative::GetGlobal) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  Local<Function> cb = Local<Function>::Cast(info[0]);

  {
    Nan::HandleScope scope;

    Local<Function> postMessageFn = Local<Function>::Cast(info.This()->Get(JS_STR("postMessage")));
    Local<Object> messageObj = Nan::New<Object>();
    messageObj->Set(JS_STR("method"), JS_STR("lock"));
    Local<Value> argv[] = {
      messageObj,
    };
    postMessageFn->Call(Nan::Null(), sizeof(argv)/sizeof(argv[0]), argv);
  }

  uv_sem_wait(vmOne->lockRequestSem);

  {
    Nan::HandleScope scope;

    Local<Value> argv[] = {
      Nan::New(vmOne->result),
    };
    cb->Call(Nan::Null(), sizeof(argv)/sizeof(argv[0]), argv);
  }

  uv_sem_post(vmOne->lockResponseSem);
} */

NAN_METHOD(WorkerNative::Request) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  uv_sem_wait(vmOne->requestSem);
}

NAN_METHOD(WorkerNative::Respond) {
  WorkerNative *vmOne = ObjectWrap::Unwrap<WorkerNative>(info.This());
  uv_sem_post(vmOne->requestSem);
}

NAN_METHOD(GetEventLoop) {
  info.GetReturnValue().Set(pointerToArray(eventLoop));
}

NAN_METHOD(SetEventLoop) {
  if (info[0]->IsArray()) {
    eventLoop = (uv_loop_t *)arrayToPointer(Local<Array>::Cast(info[0]));
  } else {
    Nan::ThrowError("SetEventLoop: invalid arguments");
  }
}

void Init(Handle<Object> exports) {
  exports->Set(JS_STR("WorkerNative"), WorkerNative::Initialize());
  Nan::SetMethod(exports, "getEventLoop", GetEventLoop);
  Nan::SetMethod(exports, "setEventLoop", SetEventLoop);
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
