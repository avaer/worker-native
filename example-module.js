console.log('example module 1');

const vmOne = requireNative('worker-native');

console.log('example module 2', typeof vmOne);

importScripts('file://./example-import.js');

console.log('example module 3');
