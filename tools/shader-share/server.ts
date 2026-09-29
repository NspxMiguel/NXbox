// NXbox shared shader cache server: one OpenGL pipeline cache per title.
//   GET /<title_id>/opengl.bin  -> the cache, or 404
//   PUT /<title_id>/opengl.bin  -> stored when it is a valid cache larger than the current one
// Consoles merge the shared cache into theirs before uploading, so a larger upload is a superset.
// bun server.ts [--port 8791] [--dir ./data]
import { mkdir, rename } from 'node:fs/promises';

const args = Bun.argv.slice(2);
const flag = (name: string, fallback: string) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : fallback;
};
const port = Number(flag('--port', '8791'));
const dir = flag('--dir', `${import.meta.dir}/data`);
const maxBytes = 64 * 1024 * 1024;
const magic = new TextEncoder().encode('yuzucach');

await mkdir(dir, { recursive: true });

function validCache(bytes: Uint8Array): boolean {
  if (bytes.length <= 12 || bytes.length > maxBytes) return false;
  return magic.every((b, i) => bytes[i] === b);
}

Bun.serve({
  port,
  maxRequestBodySize: maxBytes,
  async fetch(req) {
    const match = new URL(req.url).pathname.match(/^\/([0-9a-f]{16})\/opengl\.bin$/);
    if (!match) return new Response('not found', { status: 404 });
    const path = `${dir}/${match[1]}.bin`;
    const file = Bun.file(path);
    if (req.method === 'GET') {
      return (await file.exists()) ? new Response(file) : new Response('no cache yet', { status: 404 });
    }
    if (req.method === 'PUT') {
      const bytes = new Uint8Array(await req.arrayBuffer());
      if (!validCache(bytes)) return new Response('invalid cache', { status: 400 });
      const current = (await file.exists()) ? file.size : 0;
      if (bytes.length <= current) return new Response('kept existing', { status: 200 });
      const tmp = `${path}.${crypto.randomUUID()}.tmp`;
      await Bun.write(tmp, bytes);
      await rename(tmp, path);
      console.log(new Date().toISOString(), match[1], current, '->', bytes.length);
      return new Response('stored', { status: 201 });
    }
    return new Response('method not allowed', { status: 405 });
  },
});
console.log(`shader-share on :${port}, data in ${dir}`);
