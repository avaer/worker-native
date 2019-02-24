const {EventEmitter} = require('events');
const path = require('path');
const fs = require('fs');
const vm = require('vm');
const util = require('util');
const {Worker, workerData, parentPort} = require('worker_threads');

// latch parent WorkerNative

const {
  WorkerNative: workerNative,
  RequestContext: requestContext,
}  = (() => {
  const exports = {};
  const childVmOne = require(__dirname, 'build', 'Release', 'worker_native2.node');
  childVmOne.initChild(workerData.initFunctionAddress, exports);
  // delete require.cache[childVmOneSoPath]; // cannot be reused
  return exports;
})();

const eventLoopNative = require('event-loop-native');
workerNative.setEventLoop(eventLoopNative);
workerNative.dlclose(eventLoopNative.getDlibPath());

const v = workerNative.fromArray(workerData.array);

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

global.windowEmit = (type, event, transferList) => parentPort.postMessage({
  method: 'emit',
  type,
  event,
}, transferList);

const topRequestContext = requestContext.getTopRequestContext();
global.runSyncTop = (jsString, arg) => {
  topRequestContext.pushSyncRequest(JSON.stringify({
    method: 'runSync',
    jsString,
    arg,
  }));
  const {err, result} = JSON.parse(topRequestContext.popResult());
  if (!err) {
    return result;
  } else {
    throw new Error(err);
  }
};

global.requireNative = workerNative.requireNative;

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
    /* case 'runRepl': {
      let result, err;
      try {
        result = util.inspect(eval(m.jsString));
      } catch(e) {
        err = e.stack;
      }
      v.pushResult(JSON.stringify({result, err}));
      break;
    } */
    case 'runRepl': {
      let result, err;
      try {
        result = util.inspect(eval(m.jsString));
      } catch(e) {
        err = e.stack;
      }
      v.queueAsyncResponse(m.requestKey, JSON.stringify({result, err}));
      break;
    }
    case 'runSync': {
      let result, err;
      try {
        window._ = m.arg;
        result = eval(m.jsString);
      } catch(e) {
        err = e.stack;
      } finally {
        window._ = undefined;
      }
      v.pushResult(JSON.stringify({result, err}));
      break;
    }
    case 'runAsync': {
      let result, err;
      try {
        window._ = m.arg;
        result = eval(m.jsString);
      } catch(e) {
        err = e.stack;
      } finally {
        window._ = undefined;
      }
      if (!err) {
        Promise.resolve(result)
          .then(result => {
            v.queueAsyncResponse(m.requestKey, JSON.stringify({result}));
          });
      } else {
        v.queueAsyncResponse(m.requestKey, JSON.stringify({err}));
      }
      break;
    }
    case 'runDetached': {
      try {
        window._ = m.arg;
        eval(m.jsString);
      } catch(err) {
        console.warn(err.stack);
      } finally {
        window._ = undefined;
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
