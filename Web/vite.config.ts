import { defineConfig } from 'vite';

// Relative base so the built bundle works when the desktop shell loads it from disk
// rather than from a server. Phase 4 embeds dist/ as webview resources.
export default defineConfig({
  base: './',
  build: { outDir: 'dist', emptyOutDir: true, target: 'es2022' },
  server: { port: 5173, strictPort: true },
});
