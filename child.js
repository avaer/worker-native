const {EventEmitter} = require('events');
const path = require('path');
const fs = require('fs');
const vm = require('vm');
const {Worker, workerData, parentPort} = require('worker_threads');

// latch parent WorkerNative

const vmOne = (() => {
  const exports = {};
  const childVmOneSoPath = require.resolve(path.join(__dirname, 'build', 'Release', 'worker_native2.node'));
  const childVmOne = require(childVmOneSoPath);
  childVmOne.initChild(workerData.initFunctionAddress, exports);
  delete require.cache[childVmOneSoPath]; // cannot be reused

  return exports.WorkerNative;
})();
const v = vmOne.fromArray(workerData.array);

// global initialization

for (const k in EventEmitter.prototype) {
  global[k] = EventEmitter.prototype[k];
}
EventEmitter.call(global);

global.postMessage = (message, transferList) => parentPort.postMessage({
  method: 'postMessage',
  message,
}, transferList);
Object.defineProperty(global, 'onmessage', {
  get() {
    return this.listeners('message')[0];
  },
  set(onmessage) {
    global.on('message', onmessage);
  },
});
global.postInternalMessage = (message, transferList) => parentPort.postMessage({
  method: 'postInternalMessage',
  message,
}, transferList);
Object.defineProperty(global, 'oninternalmessage', {
  get() {
    return this.listeners('internalmessage')[0];
  },
  set(oninternalmessage) {
    global.on('internalmessage', oninternalmessage);
  },
});
global.requireNative = vmOne.requireNative;

let baseUrl = '';
function setBaseUrl(newBaseUrl) {
  baseUrl = newBaseUrl;
}
global.setBaseUrl = setBaseUrl;

const _normalizeUrl = src => {
  if (!/^(?:data|blob):/.test(src)) {
    const match = baseUrl.match(/^(file:\/\/)(.*)$/);
    if (match) {
      return match[1] + path.join(match[2], src);
    } else {
      return new URL(src, baseUrl).href;
    }
  } else {
    return src;
  }
};
function getScript(url) {
  let match;
  if (match = url.match(/^data:.+?(;base64)?,(.*)$/)) {
    if (match[1]) {
      return Buffer.from(match[2], 'base64').toString('utf8');
    } else {
      return match[2];
    }
  } else if (match = url.match(/^file:\/\/(.*)$/)) {
    return fs.readFileSync(match[1], 'utf8');
  } else {
    const sab = new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT*2 + 5 * 1024 * 1024);
    const int32Array = new Int32Array(sab);
    const worker = new Worker(path.join(__dirname, 'request.js'), {
      workerData: {
        url: _normalizeUrl(url),
        int32Array,
      },
    });
    worker.on('error', err => {
      console.warn(err.stack);
    });
    Atomics.wait(int32Array, 0, 0);
    const status = new Uint32Array(sab, 0, 1)[0];
    const length = new Uint32Array(sab, Int32Array.BYTES_PER_ELEMENT, 1)[0];
    const result = Buffer.from(sab, Int32Array.BYTES_PER_ELEMENT*2, length).toString('utf8');
    if (status === 1) {
      return result;
    } else {
      throw new Error(`fetch ${url} failed (${JSON.stringify(status)}): ${result}`);
    }
  }
}
function importScripts() {
  for (let i = 0; i < arguments.length; i++) {
    const importScriptPath = arguments[i];
    const importScriptSource = getScript(importScriptPath);
    vm.runInThisContext(importScriptSource, global, {
      filename: /^https?:/.test(importScriptPath) ? importScriptPath : 'data-url://',
    });
  }
}
global.importScripts = importScripts;

parentPort.on('message', m => {
  switch (m.method) {
    /* case 'lock': {
      v.pushResult(global);
      break;
    } */
    case 'runSync': {
      let result;
      try {
        const fn = eval(`(function(arg) { ${m.jsString} })`);
        const resultValue = fn(m.arg);
        result = JSON.stringify(resultValue !== undefined ? resultValue : null);
      } catch(err) {
        console.warn(err.stack);
      }
      console.log('push', result);
      v.pushResult(result);
      break;
    }
    case 'runAsync': {
      let result;
      try {
        const fn = eval(`(function(arg) { ${m.jsString} })`);
        const resultValue = fn(m.arg);
        result = JSON.stringify(resultValue !== undefined ? resultValue : null);
      } catch(err) {
        console.warn(err.stack);
      }
      v.queueAsyncResponse(m.requestKey, result);
      break;
    }
    case 'runDetached': {
      try {
        const fn = eval(`(function(arg) { ${m.jsString} })`);
        fn(m.arg);
      } catch(err) {
        console.warn(err.stack);
      }
      break;
    }
    case 'postMessage': {
      try {
        global.emit('message', m.message);
      } catch(err) {
        console.warn(err.stack);
      }
      break;
    }
    default: throw new Error(`invalid method: ${JSON.stringify(m.method)}`);
  }
});

// release lock

v.respond();

// run init module

if (workerData.args) {
  global.args = workerData.args;
}
if (workerData.initModule) {
  require(workerData.initModule);
}
