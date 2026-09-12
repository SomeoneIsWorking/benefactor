import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../platforms/web/disk_setup.js", import.meta.url), "utf8");
const hashes = [...source.matchAll(/sha256: "([0-9a-f]{64})"/g)].map((match) => match[1]);
assert.equal(hashes.length, 3, "the shipping picker must name three disk identities");

function fixture({ startResult = 0, failWriteAt = null } = {}) {
  const writes = [];
  const starts = [];
  const events = [];
  let onChange;
  let digestIndex = 0;
  const input = {
    disabled: false,
    files: [],
    value: "",
    addEventListener(name, handler) {
      assert.equal(name, "change");
      onChange = handler;
    },
    async change(files) {
      this.files = files;
      return onChange();
    },
  };
  const status = { textContent: "Select disks" };
  const window = {
    dispatchEvent(event) { events.push(event.type); },
  };
  const Module = {
    FS: {
      writeFile(name, bytes) {
        if (name === failWriteAt) throw new Error("guest filesystem write failed");
        writes.push([name, bytes.byteLength]);
      },
    },
    ccall(name) {
      starts.push(name);
      return startResult;
    },
  };
  const context = {
    document: {
      getElementById(id) { return id === "disk-files" ? input : status; },
    },
    window,
    Module,
    CustomEvent: class {
      constructor(type) { this.type = type; }
    },
    crypto: {
      subtle: {
    async digest() {
          const hex = hashes[digestIndex++ % hashes.length];
          return Uint8Array.from(hex.match(/../g), (byte) => parseInt(byte, 16)).buffer;
        },
      },
    },
  };
  vm.runInNewContext(source, context, { filename: "disk_setup.js" });
  const files = [1, 2, 3].map((number) => ({
    name: `Disk.${number}`,
    async arrayBuffer() { return new Uint8Array(1003520).buffer; },
  }));
  return { input, status, window, writes, starts, events, files };
}

test("a successful disk start is one-shot; reselection requires reload", async () => {
  const { input, status, window, writes, starts, events, files } = fixture();
  await input.change(files);
  assert.equal(starts.length, 1);
  assert.deepEqual(writes.map(([name]) => name), ["/Disk.1", "/Disk.2", "/Disk.3"]);
  assert.deepEqual(events, ["benefactor-disks-validated"]);
  assert.ok(window.benefactorDiskSelection.getLastValid());

  await input.change(files);
  assert.equal(starts.length, 1);
  assert.equal(writes.length, 3, "a running game must never get its disks replaced in place");
  assert.equal(input.disabled, true);
  assert.match(status.textContent, /reload/i);
});

test("a failed native start cannot silently commit a disk set or retry partial state", async () => {
  const { input, status, window, writes, starts, files } = fixture({ startResult: -1 });
  await input.change(files);
  assert.equal(starts.length, 1);
  assert.equal(writes.length, 3);
  assert.ok(!window.benefactorDiskSelection.getLastValid(), "a failed start must not publish a selection");
  assert.equal(input.disabled, true);
  assert.match(status.textContent, /reload/i);

  await input.change(files);
  assert.equal(starts.length, 1);
  assert.equal(writes.length, 3);
});

test("a disk validation failure leaves browsing available and never writes guest files", async () => {
  const { input, status, writes, starts, files } = fixture();
  await input.change(files.slice(0, 1));
  assert.equal(input.disabled, false);
  assert.match(status.textContent, /all three files are required/i);
  assert.equal(writes.length, 0);
  assert.equal(starts.length, 0);
});

test("a partial guest filesystem write requires reload before another selection", async () => {
  const { input, status, writes, starts, files } = fixture({ failWriteAt: "/Disk.2" });
  await input.change(files);
  assert.deepEqual(writes.map(([name]) => name), ["/Disk.1"]);
  assert.equal(starts.length, 0);
  assert.equal(input.disabled, true);
  assert.match(status.textContent, /reload/i);
  await input.change(files);
  assert.equal(writes.length, 1);
});

test("a second selection during asynchronous validation cannot race the first", async () => {
  const { input, writes, starts, files } = fixture();
  let releaseRead;
  files[0].arrayBuffer = () => new Promise((resolve) => {
    releaseRead = () => resolve(new Uint8Array(1003520).buffer);
  });
  const first = input.change(files);
  await input.change(files);
  assert.equal(writes.length, 0);
  releaseRead();
  await first;
  assert.equal(starts.length, 1);
  assert.equal(writes.length, 3);
});
