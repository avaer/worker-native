const path = require('path');
const {EventEmitter} = require('events');
const {Worker} = require('worker_threads');
const {
  WorkerNative: nativeWorkerNative,
  RequestContext: nativeRequestContext,
} =
  typeof requireNative === 'undefined' ?
    require(path.join(__dirname, 'build', 'Release', 'worker_native.node'))
  :
    requireNative('worker_native.node');
const vmOne2SoPath = require.resolve(path.join(__dirname, 'build', 'Release', 'worker_native2.node'));
const childJsPath = path.join(__dirname, 'child.js');

const eventLoopNative = require('event-loop-native');
nativeWorkerNative.setEventLoop(eventLoopNative);
nativeWorkerNative.dlclose(eventLoopNative.getDlibPath());
nativeWorkerNative.setNativeRequire('event_loop_native_napi.node', eventLoopNative.initFunctionAddress);

/* let compiling = false;
const make = () => new VmOne(e => {
  if (e === 'compilestart') {
    compiling = true;
  } else if (e === 'compileend') {
    compiling = false;
  }
}, __dirname + path.sep);
const isCompiling = () => compiling; */

class NativeWorker extends EventEmitter {
  constructor(options = {}) {
    super();

    const instance = new nativeWorkerNative();

    const worker = new Worker(childJsPath, {
      workerData: {
        initFunctionAddress: nativeWorkerNative.initFunctionAddress,
        array: instance.toArray(),
        initModule: options.initModule,
        args: options.args,
      },
    });
    worker.on('message', m => {
      switch (m.method) {
        case 'postMessage': {
          this.emit('message', m);
          break;
        }
        case 'emit': {
          this.emit(m.type, m.event);
          break;
        }
        default: {
          throw new Error(`worker got unknown message type '${m.method}'`);
          break;
        }
      }
    });
    worker.on('error', err => {
      this.emit('error', err);
    });
    instance.request();
    nativeWorkerNative.dlclose(vmOne2SoPath); // so we can re-require the module from a different child

    this.instance = instance;
    this.worker = worker;
  }

  runSync(jsString, arg, transferList) {
    this.worker.postMessage({
      method: 'runSync',
      jsString,
      arg,
    }, transferList);
    const {err, result} = JSON.parse(this.instance.popResult());
    if (!err) {
      return result;
    } else {
      throw new Error(err);
    }
  }
  runRepl(jsString, transferList) {
    return new Promise((accept, reject) => {
      const requestKey = this.instance.queueAsyncRequest(s => {
        const o = JSON.parse(s);
        if (!o.err) {
          accept(o.result);
        } else {
          reject(o.err);
        }
      });
      this.worker.postMessage({
        method: 'runRepl',
        jsString,
        requestKey,
      }, transferList);
    });
  }
  runAsync(jsString, arg, transferList) {
    return new Promise((accept, reject) => {
      const requestKey = this.instance.queueAsyncRequest(s => {
        const o = JSON.parse(s);
        if (!o.err) {
          accept(o.result);
        } else {
          reject(o.err);
        }
      });
      this.worker.postMessage({
        method: 'runAsync',
        jsString,
        arg,
        requestKey,
      }, transferList);
    });
  }
  runDetached(jsString, arg, transferList) {
    this.worker.postMessage({
      method: 'runDetached',
      jsString,
      arg,
    }, transferList);
  }
  postMessage(message, transferList) {
    this.worker.postMessage({
      method: 'postMessage',
      message,
    }, transferList);
  }

  get onmessage() {
    return this.listeners('message')[0];
  }
  set onmessage(onmessage) {
    this.on('message', onmessage);
  }

  get onerror() {
    return this.listeners('error')[0];
  }
  set onerror(onerror) {
    this.on('error', onerror);
  }
}

class RequestContext {
  constructor(instance = new nativeRequestContext()) {
    this.instance = instance;
  }
  
  setSyncHandler(fn) {
    this.instance.setSyncHandler(s => {
      const m = JSON.parse(s);
      let result, err;
      try {
        result = fn(m);
      } catch(e) {
        err = e.stack;
      }
      this.instance.pushResult(JSON.stringify({result, err}));
    });
  }
  
  pushSyncRequest(o) {
    this.instance.pushSyncRequest(JSON.stringify(o));
  }
}

const vmOne = {
  make(options = {}) {
    return new NativeWorker(options);
  },
  makeRequestContext() {
    return new RequestContext();
  },
  getEventLoop: nativeWorkerNative.getEventLoop,
  setEventLoop: nativeWorkerNative.setEventLoop,
  getTopRequestContext() {
    return new RequestContext(nativeRequestContext.getTopRequestContext());
  },
  setTopRequestContext(requestContext) {
    nativeRequestContext.setTopRequestContext(requestContext.instance);
  },
  setNativeRequire: nativeWorkerNative.setNativeRequire,
  requireNative: nativeWorkerNative.requireNative,
  initFunctionAddress: nativeWorkerNative.initFunctionAddress,
  fromArray(arg) {
    return new nativeWorkerNative(arg);
  },
  dlclose: nativeWorkerNative.dlclose,
}

module.exports = vmOne;
