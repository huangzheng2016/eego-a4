/// <reference types="vitest" />
import { defineConfig, loadEnv } from 'vite';
import react from '@vitejs/plugin-react';

// The production build is loaded cross-origin by the device's loader page
// (see loader/index.html), so emit stable, unhashed file names and a single
// JS chunk. `base: './'` keeps any asset references relative to app.js.
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '');
  const device = env.VITE_DEVICE || 'http://192.168.4.1';
  return {
    base: './',
    plugins: [react()],
    define: env.VITE_MOCK === '1' ? { 'import.meta.env.VITE_REPO_ROOT': JSON.stringify(new URL('..', import.meta.url).pathname.replace(/\/$/, '')) } : {},
    build: {
      target: 'es2020',
      cssCodeSplit: false,
      sourcemap: false,
      rollupOptions: {
        output: {
          inlineDynamicImports: true,
          entryFileNames: 'assets/app.js',
          chunkFileNames: 'assets/[name].js',
          assetFileNames: (info) =>
            info.names?.some((n) => n.endsWith('.css')) ? 'assets/app.css' : 'assets/[name][extname]',
        },
      },
    },
    server: {
      // Mock mode reads ../firmware/presets for the template library.
      fs: { allow: ['..'] },
      proxy: env.VITE_MOCK === '1'
        ? undefined
        : {
            '/api': {
              target: device,
              changeOrigin: true,
              configure: (proxy) => {
                // The device rejects foreign Origin headers (DNS-rebinding guard).
                proxy.on('proxyReq', (req) => req.removeHeader('origin'));
              },
            },
          },
    },
    test: {
      environment: 'node',
      include: ['test/**/*.test.ts'],
    },
  };
});
