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
      '/v1': { target: 'http://127.0.0.1:8787', changeOrigin: true, ws: true },
    },
  },
})
