import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwind from '@tailwindcss/vite'
import { fileURLToPath, URL } from 'node:url'

export default defineConfig({
  plugins: [react(), tailwind()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  server: {
    port: 5173,
    // admind binds loopback and has no authentication yet, so the dev server
    // proxies rather than the browser talking to it across origins.
    proxy: {
      // The stream binds its own port because an upgrade served from the HTTP
      // acceptor needs a hand-off admind's Server does not have. Listed first:
      // these keys are matched in order, and '/v1' would otherwise swallow it.
      '/v1/stream': { target: 'ws://127.0.0.1:8788', ws: true },
      '/v1': { target: 'http://127.0.0.1:8787', changeOrigin: true },
    },
  },
})
