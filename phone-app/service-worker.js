/*
 * 小板伴家长控制台 —— 离线外壳
 *
 * 只缓存页面本身（HTML / mqtt.js / 图标），让「装到桌面」之后没网也能打开。
 * MQTT 走的是 WebSocket，不经过 fetch，service worker 碰不到它 —— 连不连得上
 * broker 跟这里无关。
 *
 * 改了 phone-app 里的文件记得把 CACHE 的版本号 +1，否则手机上还是旧的。
 */

var CACHE = 'kidbuddy-phone-v3';

var SHELL = [
  './',
  './index.html',
  './manifest.json',
  './mqtt.min.js',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/apple-touch-icon.png'
];

self.addEventListener('install', function (e) {
  e.waitUntil(
    caches.open(CACHE)
      .then(function (c) { return c.addAll(SHELL); })
      .then(function () { return self.skipWaiting(); })
  );
});

self.addEventListener('activate', function (e) {
  e.waitUntil(
    caches.keys().then(function (keys) {
      return Promise.all(keys.map(function (k) {
        if (k !== CACHE) return caches.delete(k);
      }));
    }).then(function () { return self.clients.claim(); })
  );
});

self.addEventListener('fetch', function (e) {
  var req = e.request;

  // 只接管本站的 GET；跨域的一律放行
  if (req.method !== 'GET') return;
  if (new URL(req.url).origin !== self.location.origin) return;
  // 账号接口绝不能进缓存：里面是带会话的数据，缓存后换个人打开还是上一个人的
  if (new URL(req.url).pathname.indexOf('/api/') === 0) return;

  // 页面本体走「网络优先」：在线时总能拿到最新版，离线才吃缓存
  if (req.mode === 'navigate') {
    e.respondWith(
      fetch(req)
        .then(function (res) {
          var copy = res.clone();
          caches.open(CACHE).then(function (c) { c.put('./index.html', copy); });
          return res;
        })
        .catch(function () {
          return caches.match('./index.html').then(function (r) {
            return r || caches.match('./');
          });
        })
    );
    return;
  }

  // 其余静态资源走「缓存优先」，命中就直接回，省流量也更快
  e.respondWith(
    caches.match(req).then(function (hit) {
      if (hit) return hit;
      return fetch(req).then(function (res) {
        if (res && res.status === 200 && res.type === 'basic') {
          var copy = res.clone();
          caches.open(CACHE).then(function (c) { c.put(req, copy); });
        }
        return res;
      });
    })
  );
});
