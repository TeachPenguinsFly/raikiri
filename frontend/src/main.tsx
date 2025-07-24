import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter, Route, Routes } from 'react-router'
import { Chrome } from '@/components/Chrome'
import { Instruments } from '@/routes/Instruments'
import { Login } from '@/routes/Login'
import { Options } from '@/routes/Options'
import { Overview } from '@/routes/Overview'
import { Scanner } from '@/routes/Scanner'
import { Treasury } from '@/routes/Treasury'
import './index.css'

const root = document.getElementById('root')
if (root === null) throw new Error('#root is missing from index.html')

createRoot(root).render(
  <StrictMode>
    <BrowserRouter>
      <Routes>
        <Route path="login" element={<Login />} />
        <Route element={<Chrome />}>
          <Route index element={<Scanner />} />
          <Route path="gateways" element={<Overview />} />
          <Route path="treasury" element={<Treasury />} />
          <Route path="instruments" element={<Instruments />} />
          <Route path="options" element={<Options />} />
        </Route>
      </Routes>
    </BrowserRouter>
  </StrictMode>,
)
