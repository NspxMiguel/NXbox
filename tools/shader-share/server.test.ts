import { afterAll, expect, test, beforeAll } from 'bun:test';
const port = 18791;
let server: ReturnType<typeof Bun.spawn>;
afterAll(() => server.kill());
beforeAll(async () => {
  server = Bun.spawn(['bun', `${import.meta.dir}/server.ts`, '--port', String(port), '--dir', `/tmp/shader-share-test-${Date.now()}`]);
  await Bun.sleep(400);
});
const url = `http://127.0.0.1:${port}/01005ca01580e000/opengl.bin`;
const cache = (extra: number) => new Uint8Array([...new TextEncoder().encode('yuzucach'), 15, 0, 0, 0, ...new Array(extra).fill(7)]);
test('404 before any upload', async () => expect((await fetch(url)).status).toBe(404));
test('rejects non-cache bodies', async () => expect((await fetch(url, { method: 'PUT', body: 'hello world, not a cache' })).status).toBe(400));
test('stores a cache and serves it back', async () => {
  expect((await fetch(url, { method: 'PUT', body: cache(10) })).status).toBe(201);
  expect((await (await fetch(url)).arrayBuffer()).byteLength).toBe(22);
});
test('keeps the larger cache', async () => {
  expect((await fetch(url, { method: 'PUT', body: cache(3) })).status).toBe(200);
  expect((await (await fetch(url)).arrayBuffer()).byteLength).toBe(22);
});
test('rejects bad paths', async () => expect((await fetch(`http://127.0.0.1:${port}/../etc/passwd`)).status).toBe(404));
