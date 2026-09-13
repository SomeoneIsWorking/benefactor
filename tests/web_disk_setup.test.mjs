// The browser's chooser bridge: opening the picker, moving what the player
// chose into the module's filesystem, and answering the native pick exactly
// once. Identity and archives are the product's business and are checked from
// the native side, so nothing here may judge a file.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../platforms/web/disk_setup.js", import.meta.url), "utf8");
const pickerSource = source.slice(source.indexOf("(() => {"));
assert.ok(pickerSource.length > 0, "the shipping picker module must be readable");

function fixture({ failWriteAt = null } = {}) {
  const calls = [];
  const writes = [];
  const clicks = [];
  let changeHandler = null;
  let cancelHandler = null;
  let status = "Select disks";
  const statusElement = {
    get textContent() {
      return status;
    },
    set textContent(value) {
      status = value;
    },
  };
  const input = {
    id: "",
    type: "",
    multiple: false,
    accept: "",
    style: {},
    files: [],
    value: "stale",
    click() {
      clicks.push(this.id);
    },
    addEventListener(name, handler) {
      if (name === "change") {
        changeHandler = handler;
      } else if (name === "cancel") {
        cancelHandler = handler;
      } else {
        throw new Error(`unexpected listener ${name}`);
      }
    },
  };
  const context = {
    document: {
      getElementById(id) {
        assert.equal(id, "disk-status");
        return statusElement;
      },
      createElement(tag) {
        assert.equal(tag, "input");
        return input;
      },
      body: { appendChild() {} },
    },
    Module: {
      FS: {
        mkdirTree(name) {
          writes.push(["mkdir", name]);
        },
        writeFile(name, bytes) {
          if (name === failWriteAt) {
            throw new Error("guest filesystem write failed");
          }
          writes.push([name, bytes.byteLength]);
        },
      },
      ccall(name, _returnType, signature, args) {
        calls.push({ name, signature, args });
        return null;
      },
    },
  };
  const globalObject = vm.runInNewContext("globalThis", context, { filename: "disk_setup.js" });
  vm.runInNewContext(pickerSource, context, { filename: "disk_setup.js" });
  return {
    context,
    globalObject,
    input,
    clicks,
    calls,
    writes,
    get status() {
      return status;
    },
    pick(directory) {
      globalObject.benefactorWebPickFiles(directory);
    },
    async change(files) {
      input.files = files;
      await changeHandler();
    },
    async cancel() {
      await cancelHandler();
    },
  };
}

function diskFile(number, bytes = 1003520) {
  return {
    name: `Disk.${number}`,
    size: bytes,
    async arrayBuffer() {
      return new Uint8Array(bytes).buffer;
    },
  };
}

function names(calls) {
  return calls.map((call) => call.name);
}

test("a pick stages every chosen file and answers the native side once", async () => {
  const picker = fixture();
  picker.pick("/benefactor-data/import");
  // The chooser is created on demand and opened; nothing is answered before the
  // player makes a choice, or the screen would resume with an empty set.
  assert.deepEqual(picker.clicks, ["disk-files"]);
  assert.equal(picker.input.type, "file");
  assert.equal(picker.input.multiple, true);
  assert.deepEqual(picker.calls, []);

  await picker.change([diskFile(1), diskFile(2), diskFile(3)]);
  assert.deepEqual(names(picker.calls), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_add",
    "benefactor_web_pick_add",
    "benefactor_web_pick_add",
    "benefactor_web_pick_end",
  ]);
  // The argument arrays come from the page's own realm, so compare their text.
  assert.deepEqual(
    picker.calls.slice(1, 4).map((call) => call.args.join(",")),
    ["Disk.1", "Disk.2", "Disk.3"],
  );
  // Document names cross as C strings; the directory never comes back from the
  // page, so the native side owns the path it wrote to.
  assert.deepEqual(picker.calls[1].signature.join(","), "string");
  assert.deepEqual(picker.calls[0].signature.join(","), "");
  assert.deepEqual(picker.writes, [
    ["mkdir", "/benefactor-data/import"],
    ["/benefactor-data/import/Disk.1", 1003520],
    ["/benefactor-data/import/Disk.2", 1003520],
    ["/benefactor-data/import/Disk.3", 1003520],
  ]);
  assert.equal(picker.input.value, "", "the chooser must forget its last value");
  assert.equal(picker.status, "");
});

test("a reused chooser keeps answering later picks", async () => {
  const picker = fixture();
  picker.pick("/first");
  await picker.change([diskFile(1)]);
  picker.pick("/second");
  assert.deepEqual(picker.clicks, ["disk-files", "disk-files"]);
  await picker.change([diskFile(2)]);
  assert.deepEqual(picker.writes.slice(2), [
    ["mkdir", "/second"],
    ["/second/Disk.2", 1003520],
  ]);
  assert.equal(names(picker.calls).filter((name) => name === "benefactor_web_pick_end").length, 2);
});

test("cancelling the chooser answers the native side with no files", async () => {
  const picker = fixture();
  picker.pick("/benefactor-data/import");
  await picker.cancel();
  assert.deepEqual(names(picker.calls), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_end",
  ]);
  assert.deepEqual(picker.writes, []);
  assert.match(picker.status, /no files were chosen/i);
});

test("an empty change is a cancelled pick, and a later choice still counts", async () => {
  const picker = fixture();
  picker.pick("/benefactor-data/import");
  await picker.change([]);
  assert.deepEqual(names(picker.calls), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_end",
  ]);
  // A browser that cannot display the dialog reports the dismissal and then the
  // selection, so the selection is what the product must act on.
  await picker.change([diskFile(1)]);
  assert.deepEqual(names(picker.calls).slice(2), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_add",
    "benefactor_web_pick_end",
  ]);
  assert.deepEqual(picker.writes, [
    ["mkdir", "/benefactor-data/import"],
    ["/benefactor-data/import/Disk.1", 1003520],
  ]);
});

test("files beyond the import budget are refused before the filesystem sees them", async () => {
  const picker = fixture();
  picker.pick("/benefactor-data/import");
  await picker.change([diskFile(1, 1003520), diskFile(2, 100 * 1024 * 1024)]);
  assert.deepEqual(names(picker.calls), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_end",
  ]);
  assert.deepEqual(picker.writes, []);
  assert.match(picker.status, /larger than this setup accepts/i);
});

test("a failed filesystem write is reported and still answers the native side", async () => {
  const picker = fixture({ failWriteAt: "/benefactor-data/import/Disk.2" });
  picker.pick("/benefactor-data/import");
  await picker.change([diskFile(1), diskFile(2)]);
  assert.match(picker.status, /write failed/i);
  assert.equal(names(picker.calls).at(-1), "benefactor_web_pick_end");
});

test("a pick with no module filesystem is refused, and the pick is still answered", async () => {
  const picker = fixture();
  delete picker.context.Module.FS;
  picker.pick("/benefactor-data/import");
  await picker.change([diskFile(1)]);
  assert.match(picker.status, /could not open its own filesystem/i);
  // The answer still ends: a pick that never answers would leave the screen
  // waiting for a selection that has already been made.
  assert.deepEqual(names(picker.calls), [
    "benefactor_web_pick_begin",
    "benefactor_web_pick_end",
  ]);
  assert.deepEqual(picker.writes, []);
});

test("the native status line reaches the page", () => {
  const picker = fixture();
  picker.globalObject.benefactorWebStatus("Still needed: Disk.2, Disk.3");
  assert.equal(picker.status, "Still needed: Disk.2, Disk.3");
});
