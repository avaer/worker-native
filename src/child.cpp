#include <v8.h>
#include <uv.h>
#include <nan.h>

// #include <iostream>

using namespace v8;
using namespace node;

#define JS_STR(...) Nan::New<v8::String>(__VA_ARGS__).ToLocalChecked()
#define JS_INT(val) Nan::New<v8::Integer>(val)
#define JS_NUM(val) Nan::New<v8::Number>(val)
#define JS_FLOAT(val) Nan::New<v8::Number>(val)
#define JS_BOOL(val) Nan::New<v8::Boolean>(val)

#define JS_FUNC(x) (Nan::GetFunction(x).ToLocalChecked())
#define JS_OBJ(x) (Nan::To<v8::Object>(x).ToLocalChecked())
#define JS_NUM(x) (Nan::To<double>(x).FromJust())
#define JS_BOOL(x) (Nan::To<bool>(x).FromJust())
#define JS_UINT32(x) (Nan::To<unsigned int>(x).FromJust())
#define JS_INT32(x) (Nan::To<int>(x).FromJust())
#define JS_ISOLATE() (v8::Isolate::GetCurrent())
#define JS_CONTEXT() (JS_ISOLATE()->GetCurrentContext())
#define JS__HAS(x, y) (((x)->Has(JS_CONTEXT(), (y))).FromJust())

#define EXO_ToString(x) (Nan::To<v8::String>(x).ToLocalChecked())

#define UINT32_TO_JS(x) (Nan::New(static_cast<uint32_t>(x)))
#define INT32_TO_JS(x) (Nan::New(static_cast<int32_t>(x)))
#define BOOL_TO_JS(x) ((x) ? Nan::True() : Nan::False())
#define DOUBLE_TO_JS(x) (Nan::New(static_cast<double>(x)))
#define FLOAT_TO_JS(x) (Nan::New(static_cast<float>(x)))

namespace vmone2 {

NAN_METHOD(InitChild) {
  if (info[0]->IsArray() && info[1]->IsObject()) {
    Local<Array> array = Local<Array>::Cast(info[0]);
    uint32_t a = JS_UINT32(array->Get(0));
    uint32_t b = JS_UINT32(array->Get(1));
    uintptr_t c = ((uintptr_t)a << 32) | (uintptr_t)b;
    void (*initFn)(Local<Object>) = reinterpret_cast<void (*)(Local<Object>)>(c);

    Local<Object> obj = Local<Object>::Cast(info[1]);
    (*initFn)(obj);
  } else {
    Nan::ThrowError("InitChild: Invalid argunents");
  }
}

void Init(Local<Object> exports) {
  Nan::HandleScope scope;
  
  Local<Function> initChildFn = Nan::New<Function>(InitChild);
  exports->Set(JS_STR("initChild"), initChildFn);
  
  /* uintptr_t initFunctionAddress = (uintptr_t)vmone2::Init;
  Local<Array> initFunctionAddressArray = Nan::New<Array>(2);
  initFunctionAddressArray->Set(0, Nan::New<Integer>((uint32_t)(initFunctionAddress >> 32)));
  initFunctionAddressArray->Set(1, Nan::New<Integer>((uint32_t)(initFunctionAddress & 0xFFFFFFFF)));
  exports->Set(JS_STR("initFunctionAddress"), initFunctionAddressArray); */
}

}

#ifndef LUMIN
NODE_MODULE(NODE_GYP_MODULE_NAME, vmone2::Init)
#else
extern "C" {
  void node_register_module_worker_native2(Local<Object> exports, Local<Value> module, Local<Context> context) {
    vmone2::Init(exports);
  }
}
#endif
