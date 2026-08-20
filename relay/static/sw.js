/* Deliberately minimal: this app is useless offline (it exists to reach the
 * relay), so caching the shell only risks serving a stale one. */
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
