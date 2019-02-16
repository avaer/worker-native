const path = require('path');
const {EventEmitter} = require('events');
const {Worker} = require('worker_threads');
const {WorkerNative: nativeVmOne} = typeof requireNative === 'undefined' ? require(path.join(__dirname, 'build', 'Release', 'worker_native.node')) : requireNative('worker_native.node');
const vmOne2SoPath = require.resolve(path.join(__dirname, 'build', 'Release', 'worker_native2.node'));
const childJsPath = path.join(__dirname, 'child.js');

/* let compiling = false;
const make = () => new VmOne(e => {
  if (e === 'compilestart') {
    compiling = true;
  } else if (e === 'compileend') {
    compiling = false;
  }
}, __dirname + path.sep);
const isCompiling = () => compiling; */

class Vm extends EventEmitter {
  constructor(options = {}) {
    super();

    const instance = new nativeVmOne();

    const worker = new Worker(childJsPath, {
      workerData: {
        initFunctionAddress: nativeVmOne.initFunctionAddress,
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
        case 'postInternalMessage': {
          this.emit('internalmessage', m);
          break;
        }
      }
    });
    worker.on('error', err => {
      this.emit('error', err);
    });
    instance.request();
    nativeVmOne.dlclose(vmOne2SoPath); // so we can re-require the module from a different child

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
    this.worker.postMessage({
      method: 'runRepl',
      jsString,
    }, transferList);
    const {err, result} = JSON.parse(this.instance.popResult());
    if (!err) {
      return result;
    } else {
      throw new Error(err);
    }
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

  get oninternalmessage() {
    return this.listeners('inteernalmessage')[0];
  }
  set oninternalmessage(oninternalmessage) {
    this.on('internalmessage', oninternalmessage);
  }

  get onerror() {
    return this.listeners('error')[0];
  }
  set onerror(onerror) {
    this.on('error', onerror);
  }
}

const vmOne = {
  make(options = {}) {
    return new Vm(options);
  },
  setNativeRequire: nativeVmOne.setNativeRequire,
  requireNative: nativeVmOne.requireNative,
  initFunctionAddress: nativeVmOne.initFunctionAddress,
  fromArray(arg) {
    return new nativeVmOne(arg);
  },
}

module.exports = vmOne;
