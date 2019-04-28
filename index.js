const path = require('path');
const {EventEmitter} = require('events');
const {Worker} = require('worker_threads');
const vmOnePath = path.join(__dirname, 'build', 'Release', 'worker_native.node');
const {
  WorkerNative: nativeWorkerNative,
  RequestContext: nativeRequestContext,
} = typeof requireNative === 'undefined' ?
  require(path.join(__dirname, 'build', 'Release', 'worker_native.node'))
:
  requireNative('worker_native.node');
const vmOne2SoPath = require.resolve(path.join(__dirname, 'build', 'Release', 'worker_native2.node'));
const childJsPath = path.join(__dirname, 'child.js');

nativeWorkerNative.setNativeRequire('worker_native.node', nativeWorkerNative.initFunctionAddress);

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
        case 'response': {
          const fn = this.queue[m.requestKey];

          if (fn) {
            fn(m.error, m.result);
          } else {
            console.warn(`unknown response request key: ${m.requestKey}`);
          }
          break;
        }
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
    worker.on('exit', () => {
      this.emit('exit');
    });
    instance.request();
    nativeWorkerNative.dlclose(vmOne2SoPath); // so we can re-require the module from a different child

    this.instance = instance;
    this.worker = worker;
    this.requestKeys = 0;
    this.queue = {};
  }

  queueRequest(fn) {
    const requestKey = this.requestKeys++;
    this.queue[requestKey] = fn;
    return requestKey;
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
      const requestKey = this.queueRequest((err, result) => {
        if (!err) {
          accept(result);
        } else {
          reject(err);
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
      const requestKey = this.queueRequest((err, result) => {
        if (!err) {
          accept(result);
        } else {
          reject(err);
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
  
  destroy() {
    const symbols = Object.getOwnPropertySymbols(this.worker);
    const publicPortSymbol = symbols.find(s => s.toString() === 'Symbol(kPublicPort)');
    const publicPort = this.worker[publicPortSymbol];
    publicPort.close();
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
  
  get onexit() {
    return this.listeners('exit')[0];
  }
  set onexit(onexit) {
    this.on('exit', onexit);
  }
}

class RequestContext {
  constructor(instance = new nativeRequestContext()) {
    this.instance = instance;
  }
  
  popResult() {
    return this.instance.popResult();
  }
}

const _makeRequestContext = rc => {
  const requestContext = new RequestContext(rc);
  requestContext.runSyncTop = function(method, argsBuffer) {
    this.pushSyncRequest(method, argsBuffer);
    const result = this.popResult();
    return result;
  };
  return requestContext;
};

const vmOne = {
  make(options = {}) {
    return new NativeWorker(options);
  },
  makeRequestContext() {
    return _makeRequestContext();
  },
  getEventLoop: nativeWorkerNative.getEventLoop,
  setNativeRequire: nativeWorkerNative.setNativeRequire,
  requireNative: nativeWorkerNative.requireNative,
  initFunctionAddress: nativeWorkerNative.initFunctionAddress,
  fromArray(arg) {
    return new nativeWorkerNative(arg);
  },
  dlclose: nativeWorkerNative.dlclose,
}

module.exports = vmOne;
