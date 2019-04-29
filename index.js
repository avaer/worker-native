const path = require('path');
const {EventEmitter} = require('events');
const {Worker} = require('worker_threads');
/* const {
  WorkerNative: nativeWorkerNative,
} = require(path.join(__dirname, 'build', 'Release', 'worker_native.node')); */
const childJsPath = path.join(__dirname, 'child.js');

// nativeWorkerNative.setNativeRequire('worker_native.node', nativeWorkerNative.initFunctionAddress);

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

    //const instance = new nativeWorkerNative();

    const worker = new Worker(childJsPath, {
      workerData: {
        // initFunctionAddress: nativeWorkerNative.initFunctionAddress,
        // array: instance.toArray(),
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

    // this.instance = instance;
    this.worker = worker;
    this.requestKeys = 0;
    this.queue = {};
  }

  queueRequest(fn) {
    const requestKey = this.requestKeys++;
    this.queue[requestKey] = fn;
    return requestKey;
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

module.exports = NativeWorker;
