/* GitHub Pages does not let the project set COOP/COEP response headers. The
 * same-origin worker adds them to the app responses after one bounded reload,
 * which is the browser contract Emscripten pthreads require. */
export async function prepareApplication(serviceWorker = "service-worker.js") {
  if (!globalThis.isSecureContext || !navigator.serviceWorker) {
    throw new Error("This browser needs HTTPS and service-worker support.");
  }
  await navigator.serviceWorker.register(serviceWorker, { scope: "./" });
  await navigator.serviceWorker.ready;
  if (!globalThis.crossOriginIsolated) {
    const key = `benefactor-isolation:${new URL(serviceWorker, location.href).pathname}`;
    if (sessionStorage.getItem(key)) {
      throw new Error("Browser isolation is unavailable; enable service workers and reload.");
    }
    sessionStorage.setItem(key, "requested");
    location.reload();
    return false;
  }
  sessionStorage.removeItem(`benefactor-isolation:${new URL(serviceWorker, location.href).pathname}`);
  return true;
}
