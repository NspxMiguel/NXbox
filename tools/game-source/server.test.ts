import { afterAll, beforeAll, expect, test } from 'bun:test';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHandler, listGames } from './server';

let dir = '';
let server: ReturnType<typeof Bun.serve>;
const body = new Uint8Array(1000).map((_, i) => i % 251);

beforeAll(async () => {
  dir = await mkdtemp(join(tmpdir(), 'game-source-'));
  await mkdir(join(dir, 'Some Game [NSZ]'));
  await writeFile(join(dir, 'Some Game [NSZ]', 'Some Game [0100000000010000][v0].nsz'), body);
  await writeFile(join(dir, 'notes.txt'), 'not a game');
  await writeFile(join(dir, 'other.xci'), new Uint8Array(10));
  server = Bun.serve({ port: 0, fetch: createHandler(dir) });
});

afterAll(async () => {
  server.stop(true);
  await rm(dir, { recursive: true, force: true });
});

test('lists only game files, recursively', async () => {
  const games = await listGames(dir);
  expect(games.map((g) => g.rel)).toEqual(['other.xci', 'Some Game [NSZ]/Some Game [0100000000010000][v0].nsz']);
  expect(games[1].size).toBe(1000);
});

test('index is Tinfoil shop JSON with absolute, encoded URLs', async () => {
  const index = await (await fetch(`http://localhost:${server.port}/`)).json();
  expect(index.files).toHaveLength(2);
  const game = index.files.find((f: { url: string }) => f.url.endsWith('.nsz'));
  expect(game.size).toBe(1000);
  expect(game.url).toBe(
    `http://localhost:${server.port}/files/Some%20Game%20%5BNSZ%5D/Some%20Game%20%5B0100000000010000%5D%5Bv0%5D.nsz`,
  );
  expect(typeof index.success).toBe('string');
});

test('serves a whole file and a byte range', async () => {
  const index = await (await fetch(`http://localhost:${server.port}/`)).json();
  const game = index.files.find((f: { url: string }) => f.url.endsWith('.nsz'));
  const whole = new Uint8Array(await (await fetch(game.url)).arrayBuffer());
  expect(whole).toEqual(body);
  const part = await fetch(game.url, { headers: { Range: 'bytes=100-199' } });
  expect(part.status).toBe(206);
  expect(part.headers.get('content-range')).toBe('bytes 100-199/1000');
  expect(new Uint8Array(await part.arrayBuffer())).toEqual(body.slice(100, 200));
  const tail = await fetch(game.url, { headers: { Range: 'bytes=990-' } });
  expect(new Uint8Array(await tail.arrayBuffer())).toEqual(body.slice(990));
});

test('refuses paths outside the folder and non-game files', async () => {
  expect((await fetch(`http://localhost:${server.port}/files/notes.txt`)).status).toBe(404);
  expect((await fetch(`http://localhost:${server.port}/files/..%2F..%2Fetc%2Fpasswd`)).status).toBe(404);
  expect((await fetch(`http://localhost:${server.port}/files/missing.nsp`)).status).toBe(404);
});
