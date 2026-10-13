import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwind from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwind()],
  server: {
    port: 5173,
    // admind binds loopback with no auth, so the dev server proxies it
    proxy: {
      // listed before /v1 since keys match in order; the stream has its own port, its token does not
      '^/v1/stream(\\?|$)': { target: 'ws://127.0.0.1:8788', ws: true },
      '/v1': { target: 'http://127.0.0.1:8787', changeOrigin: true },
    },
  },
})
