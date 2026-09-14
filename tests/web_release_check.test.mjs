// The browser's update-check bridge: one fetch, one answer back into the module,
// and no answer that could be read as "up to date" when nothing was learned.
//
// The name the native side calls is checked here too. Both halves of this
// boundary are in this repository, and they only meet at runtime in a browser:
// they were once spelled differently, the page answered "this page cannot check
// for updates", and nothing but a real browser ran the two together.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../platforms/web/release_check.js", import.meta.url), "utf8");
const bridgeSource = source.slice(source.indexOf("(() => {"));
assert.ok(bridgeSource.length > 0, "the shipping release-check module must be readable");
const nativeSource = readFileSync(new URL("../src/platform/web_update.cpp", import.meta.url), "utf8");

const URL_UNDER_TEST =
  "https://api.github.com/repos/SomeoneIsWorking/benefactor/releases/latest";

/** Load the bridge with one recorded fetch response and read back its answer. */
async function answerFor(respond) {
  const answers = [];
  const requests = [];
  const context = {
    fetch(url, options) {
      requests.push({ url, options });
      return respond();
    },
    Module: {
      ccall(name, returnType, signature, args) {
        answers.push({ name, returnType, signature, args });
      },
    },
  };
  context.globalThis = context;
  vm.runInNewContext(bridgeSource, context);
  assert.equal(typeof context.benefactorWebCheckRelease, "function", "the page hook must exist");
  await context.benefactorWebCheckRelease(URL_UNDER_TEST);
  assert.equal(requests.length, 1, "one request per check");
  assert.equal(requests[0].url, URL_UNDER_TEST, "the address comes from the product");
  return answers;
}

function releaseResponse(release, { ok = true } = {}) {
  return () =>
    Promise.resolve(
      ok
        ? { ok: true, json: () => Promise.resolve(release) }
        : { ok: false, status: 403, json: () => Promise.resolve({}) },
    );
}

test("a release document answers with its tag", async () => {
  const answers = await answerFor(releaseResponse({ tag_name: "v0.4.0" }));
  assert.equal(answers.length, 1, "one answer per check");
  const [answer] = answers;
  assert.equal(answer.name, "benefactor_web_update_result");
  assert.equal(answer.returnType, null, "the module call returns nothing");
  assert.deepEqual([...answer.signature], ["string", "string"]);
  assert.deepEqual([...answer.args], ["v0.4.0", ""], "the tag, and no reason");
});

test("a refused request is a reason, never a tag", async () => {
  const answers = await answerFor(releaseResponse({}, { ok: false }));
  assert.equal(answers.length, 1, "every check answers exactly once");
  assert.equal(answers[0].name, "benefactor_web_update_result");
  assert.equal(answers[0].args[0], "", "no tag");
  assert.match(
    answers[0].args[1],
    /refused/,
    "the reason names what happened so the product reports a failure",
  );
});

test("a body that cannot be read is a reason", async () => {
  const answers = await answerFor(() =>
    Promise.resolve({ ok: true, json: () => Promise.reject(new Error("truncated")) }),
  );
  assert.equal(answers.length, 1);
  assert.equal(answers[0].args[0], "", "no tag");
  assert.match(answers[0].args[1], /nothing readable/);
});

test("a document without a tag is a reason, not an answer", async () => {
  const answers = await answerFor(releaseResponse({ name: "Benefactor v0.4.0" }));
  assert.equal(answers.length, 1);
  assert.equal(answers[0].args[0], "", "no tag");
  assert.match(answers[0].args[1], /no release tag/);
});

test("a failed request is a reason", async () => {
  const answers = await answerFor(() => Promise.reject(new Error("offline")));
  assert.equal(answers.length, 1);
  assert.equal(answers[0].args[0], "", "no tag");
  assert.match(answers[0].args[1], /could not reach the network/);
});

test("the native side calls the hook this module defines", () => {
  const defined = bridgeSource.match(/globalThis\.(\w+)\s*=/);
  assert.ok(defined !== null, "the module defines a page hook");
  const hook = defined[1];
  assert.ok(
    nativeSource.includes(`window.${hook}`),
    `src/platform/web_update.cpp must call window.${hook}, the name this module defines`,
  );
});
