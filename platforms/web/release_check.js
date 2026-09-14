/* release_check.js — the browser's half of the update check.
 *
 * What a release tag means, which service to ask, and what the player sees all
 * belong to the product. What only the browser can do is make the request, so
 * this module owns exactly that: the page's fetch(), which has the browser's own
 * network stack. The product calls benefactorWebCheckRelease(url) once per run
 * with the address it owns, and this answers exactly once through the module —
 * with the tag, or with a reason the check could not run. A check that did not
 * happen must never look like an answer, so a refused request and a body without
 * a tag are both reported as failures rather than as "up to date". */
(() => {
  "use strict";

  // Crossing into the module passes strings as pointers, so the signature is
  // declared rather than left to a JavaScript coercion.
  const ANSWER = Object.freeze({
    name: "benefactor_web_update_result",
    signature: ["string", "string"],
  });

  function answer(tag, error) {
    if (typeof Module === "undefined" || typeof Module.ccall !== "function") {
      return;
    }
    Module.ccall(ANSWER.name, null, ANSWER.signature, [
      tag === null ? "" : tag,
      error === null ? "" : error,
    ]);
  }

  // One fetch, one answer. Each way this can fail says which one it was: a
  // request that never left, a service that refused it, a body that cannot be
  // read, and a document with no tag are four different reasons, and the product
  // reports the one that happened rather than a single vague failure.
  globalThis.benefactorWebCheckRelease = async (url) => {
    let response;
    try {
      response = await fetch(url, { headers: { Accept: "application/vnd.github+json" } });
    } catch {
      answer(null, "the update check could not reach the network");
      return;
    }
    if (!response.ok) {
      answer(null, "the release service refused the request");
      return;
    }
    let release;
    try {
      release = await response.json();
    } catch {
      answer(null, "the release service sent nothing readable");
      return;
    }
    const tag = typeof release.tag_name === "string" ? release.tag_name : "";
    if (tag === "") {
      answer(null, "no release tag in the response");
      return;
    }
    answer(tag, null);
  };
})();
