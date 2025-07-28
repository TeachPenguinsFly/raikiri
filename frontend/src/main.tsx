import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter, Route, Routes } from 'react-router'
import { AppShell } from '@/components/layout/AppShell'
import { Login } from '@/routes/Login'
import { MarketsPage } from '@/pages/MarketsPage'
import { SettingsPage } from '@/pages/SettingsPage'
import { StrategiesPage } from '@/pages/StrategiesPage'
import { TreasuryPage } from '@/pages/TreasuryPage'
import './index.css'

const root = document.getElementById('root')
if (root === null) throw new Error('#root is missing from index.html')

createRoot(root).render(
  <StrictMode>
    <BrowserRouter>
      <Routes>
        <Route path="login" element={<Login />} />
        <Route element={<AppShell />}>
          <Route index element={<MarketsPage />} />
          <Route path="strategies" element={<StrategiesPage />} />
          <Route path="treasury" element={<TreasuryPage />} />
          <Route path="settings" element={<SettingsPage />} />
        </Route>
      </Routes>
    </BrowserRouter>
  </StrictMode>,
)
