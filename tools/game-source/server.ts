// NXbox game source: serves a folder of the owner's own dumps in the Tinfoil shop format, so NXbox
// (Settings → Sources) can list and download them over the LAN.
//   GET /            -> {"files":[{"url","size"}],"directories":[],"success":"..."} (Tinfoil shop JSON)
//   GET /files/<rel> -> the file, with HTTP Range support so downloads resume
// Only .nsp, .nsz, .xci and .xcz files under the folder are listed; nothing outside it is served.
// bun server.ts --dir ~/Games/Switch [--port 8792]
import { readdir, stat } from 'node:fs/promises';
import { join, relative, resolve, sep } from 'node:path';

const args = Bun.argv.slice(2);
const flag = (name: string, fallback: string) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : fallback;
};

const extensions = ['.nsp', '.nsz', '.xci', '.xcz'];

export async function listGames(root: string): Promise<{ rel: string; size: number }[]> {
  const found: { rel: string; size: number }[] = [];
  const walk = async (dir: string) => {
    for (const entry of await readdir(dir, { withFileTypes: true })) {
      const full = join(dir, entry.name);
      if (entry.isDirectory()) await walk(full);
      else if (extensions.some((ext) => entry.name.toLowerCase().endsWith(ext))) {
        found.push({ rel: relative(root, full).split(sep).join('/'), size: (await stat(full)).size });
      }
    }
  };
  await walk(root);
  return found.sort((a, b) => a.rel.localeCompare(b.rel));
}

export function createHandler(root: string) {
  const base = resolve(root);
  return async (req: Request): Promise<Response> => {
    const url = new URL(req.url);
    if (url.pathname === '/' || url.pathname === '/index.json') {
      const games = await listGames(base);
      const files = games.map((g) => ({
        url: `${url.origin}/files/${g.rel.split('/').map(encodeURIComponent).join('/')}`,
        size: g.size,
      }));
      return Response.json({ files, directories: [], success: `${files.length} file(s) from this source` });
    }
    if (url.pathname.startsWith('/files/')) {
      const rel = url.pathname.slice('/files/'.length).split('/').map(decodeURIComponent).join(sep);
      const full = resolve(base, rel);
      if (!full.startsWith(base + sep) || !extensions.some((ext) => full.toLowerCase().endsWith(ext))) {
        return new Response('not found', { status: 404 });
      }
      const file = Bun.file(full);
      if (!(await file.exists())) return new Response('not found', { status: 404 });
      const size = file.size;
      const range = req.headers.get('range')?.match(/^bytes=(\d*)-(\d*)$/);
      if (range && (range[1] || range[2])) {
        let start = range[1] ? Number(range[1]) : size - Number(range[2]);
        let end = range[1] && range[2] ? Number(range[2]) : size - 1;
        start = Math.max(0, start);
        end = Math.min(end, size - 1);
        if (start > end) {
          return new Response('', { status: 416, headers: { 'Content-Range': `bytes */${size}` } });
        }
        return new Response(file.slice(start, end + 1), {
          status: 206,
          headers: {
            'Content-Range': `bytes ${start}-${end}/${size}`,
            'Content-Length': String(end - start + 1),
            'Accept-Ranges': 'bytes',
            'Content-Type': 'application/octet-stream',
          },
        });
      }
      return new Response(file, {
        headers: { 'Accept-Ranges': 'bytes', 'Content-Length': String(size), 'Content-Type': 'application/octet-stream' },
      });
    }
    return new Response('not found', { status: 404 });
  };
}

if (import.meta.main) {
  const dir = flag('--dir', '');
  if (!dir) {
    console.error('usage: bun server.ts --dir <folder with your dumps> [--port 8792]');
    process.exit(1);
  }
  const server = Bun.serve({ port: Number(flag('--port', '8792')), fetch: createHandler(dir) });
  console.log(`NXbox game source on http://0.0.0.0:${server.port}/ serving ${resolve(dir)}`);
}
