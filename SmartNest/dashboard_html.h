#pragma once
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartNest | Local Dashboard</title>
<style>
  :root {
    --primary: hsl(221, 100%, 35%);
    --primary-light: hsl(221, 83%, 53%);
    --bg-dark: hsl(224, 71%, 4%);
    --card-bg: hsl(223, 47%, 11%);
    --border: rgba(255, 255, 255, 0.08);
    --success: hsl(162, 72%, 41%);
    --text-light: #f1f5f9;
    --text-muted: #94a3b8;
    --font-stack: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  * { margin:0; padding:0; box-sizing:border-box; }
  body {
    font-family: var(--font-stack);
    background-color: var(--bg-dark);
    color: var(--text-light);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 20px;
    line-height: 1.5;
  }
  .container {
    max-width: 500px;
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 20px;
  }
  .card {
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 24px;
    box-shadow: 0 10px 30px rgba(0,0,0,0.5);
  }
  .header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 20px;
  }
  .header-title-box {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .logo-svg {
    width: 32px;
    height: 32px;
  }
  .header-title {
    font-size: 18px;
    font-weight: 700;
    letter-spacing: -0.5px;
  }
  .status-badge {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 10px;
    font-weight: 700;
    text-transform: uppercase;
    background: rgba(16, 185, 129, 0.1);
    color: var(--success);
    padding: 4px 10px;
    border-radius: 20px;
    border: 1px solid rgba(16, 185, 129, 0.2);
  }
  .status-dot {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--success);
    animation: pulse 1.8s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }
  .stats-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    margin-bottom: 20px;
  }
  .stat-box {
    background: rgba(255,255,255,0.02);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 16px;
    text-align: center;
  }
  .stat-lbl {
    font-size: 10px;
    font-weight: 700;
    color: var(--text-muted);
    text-transform: uppercase;
    margin-bottom: 4px;
    letter-spacing: 0.5px;
  }
  .stat-val {
    font-size: 22px;
    font-weight: 800;
  }
  .stat-sub {
    font-size: 11px;
    color: var(--primary-light);
    margin-top: 2px;
  }
  .controls-title {
    font-size: 11px;
    font-weight: 700;
    color: var(--text-muted);
    text-transform: uppercase;
    margin-bottom: 12px;
    letter-spacing: 0.5px;
  }
  .relay-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
  }
  .relay-card {
    background: rgba(255,255,255,0.02);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 14px 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    cursor: pointer;
    transition: all 0.2s ease;
  }
  .relay-card:hover {
    background: rgba(255,255,255,0.04);
    border-color: rgba(255,255,255,0.15);
  }
  .relay-card.active {
    background: rgba(33, 112, 228, 0.08);
    border-color: rgba(33, 112, 228, 0.3);
  }
  .relay-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .relay-name {
    font-size: 14px;
    font-weight: 600;
  }
  .relay-status {
    font-size: 11px;
    color: var(--text-muted);
  }
  .relay-card.active .relay-status {
    color: var(--primary-light);
    font-weight: 600;
  }
  .toggle-switch {
    width: 36px;
    height: 20px;
    background: hsl(223, 14%, 20%);
    border-radius: 20px;
    position: relative;
    transition: background 0.2s ease;
  }
  .toggle-switch::after {
    content: '';
    position: absolute;
    width: 14px;
    height: 14px;
    border-radius: 50%;
    background: #ffffff;
    top: 3px;
    left: 3px;
    transition: transform 0.2s ease;
  }
  .relay-card.active .toggle-switch {
    background: var(--primary-light);
  }
  .relay-card.active .toggle-switch::after {
    transform: translateX(16px);
  }
  .footer {
    text-align: center;
    font-size: 11px;
    color: var(--text-muted);
    margin-top: 10px;
  }
  .shutdown-card {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.05) 0%, rgba(220, 38, 38, 0.02) 100%);
    border: 1px solid rgba(239, 68, 68, 0.2);
    display: flex;
    justify-content: space-between;
    align-items: center;
    cursor: pointer;
    transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    margin-bottom: 20px;
    padding: 16px 20px;
  }
  .shutdown-card:hover {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.1) 0%, rgba(220, 38, 38, 0.05) 100%);
    border-color: rgba(239, 68, 68, 0.4);
    box-shadow: 0 4px 20px rgba(239, 68, 68, 0.15);
  }
  .shutdown-card.active {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.25) 0%, rgba(220, 38, 38, 0.15) 100%);
    border-color: rgba(239, 68, 68, 0.8);
    box-shadow: 0 0 25px rgba(239, 68, 68, 0.35);
    animation: alarm-pulse 2s infinite;
  }
  @keyframes alarm-pulse {
    0%, 100% { border-color: rgba(239, 68, 68, 0.8); }
    50% { border-color: rgba(239, 68, 68, 0.4); }
  }
  .shutdown-content {
    display: flex;
    align-items: center;
    gap: 14px;
  }
  .shutdown-icon {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    background: rgba(239, 68, 68, 0.1);
    color: #ef4444;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.3s ease;
  }
  .shutdown-card.active .shutdown-icon {
    background: #ef4444;
    color: #ffffff;
    transform: rotate(90deg);
  }
  .shutdown-icon svg {
    width: 18px;
    height: 18px;
  }
  .shutdown-text {
    display: flex;
    flex-direction: column;
    gap: 2px;
    text-align: left;
  }
  .shutdown-title {
    font-size: 14px;
    font-weight: 700;
    letter-spacing: -0.3px;
  }
  .shutdown-desc {
    font-size: 11px;
    color: var(--text-muted);
  }
  .shutdown-card.active .shutdown-desc {
    color: #fca5a5;
  }
  .shutdown-badge {
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
    padding: 4px 10px;
    border-radius: 20px;
    border: 1px solid rgba(239, 68, 68, 0.2);
    color: #ef4444;
    background: rgba(239, 68, 68, 0.05);
    transition: all 0.3s ease;
  }
  .shutdown-card.active .shutdown-badge {
    background: #ef4444;
    color: #ffffff;
    border-color: #ef4444;
  }
  .relay-card.locked {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.08) 0%, rgba(220, 38, 38, 0.03) 100%);
    border-color: rgba(239, 68, 68, 0.3);
  }
  .relay-card.locked:hover {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.12) 0%, rgba(220, 38, 38, 0.05) 100%);
    border-color: rgba(239, 68, 68, 0.45);
  }
  .relay-card.locked .relay-status {
    color: #ef4444 !important;
    font-weight: 700;
  }
  .relay-card.locked .toggle-switch {
    background: hsl(223, 14%, 20%) !important;
    opacity: 0.5;
  }
  .relay-card.locked .toggle-switch::after {
    transform: translateX(0) !important;
  }
  .lock-btn {
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border);
    color: var(--text-muted);
    border-radius: 8px;
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s ease;
  }
  .lock-btn:hover {
    background: rgba(255, 255, 255, 0.1);
    color: var(--text-light);
    border-color: rgba(255, 255, 255, 0.2);
  }
  .relay-card.locked .lock-btn {
    background: rgba(239, 68, 68, 0.2);
    border-color: rgba(239, 68, 68, 0.5);
    color: #ef4444;
  }
  .relay-card.locked .lock-btn:hover {
    background: rgba(239, 68, 68, 0.3);
    border-color: rgba(239, 68, 68, 0.7);
  }
  .lock-btn svg {
    width: 16px;
    height: 16px;
  }
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="header-row">
      <div class="header-title-box">
        <svg class="logo-svg" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
          <circle cx="50" cy="50" r="45" stroke="#2170e4" stroke-width="4" stroke-dasharray="8 4" fill="none"/>
          <path d="M50 20 L26 40 L26 70 A 4 4 0 0 0 30 74 L70 74 A 4 4 0 0 0 74 70 L74 40 Z" fill="none" stroke="#2170e4" stroke-width="5" stroke-linejoin="round"/>
          <circle cx="50" cy="42" r="5" fill="#10b981"/>
          <circle cx="38" cy="58" r="4" fill="#2170e4"/>
          <circle cx="62" cy="58" r="4" fill="#f59e0b"/>
          <path d="M50 47 L38 58 M50 47 L62 58" stroke="#64748b" stroke-width="2" stroke-linecap="round"/>
        </svg>
        <span class="header-title">SmartNest Local</span>
      </div>
      <div class="status-badge">
        <span class="status-dot"></span>
        <span>Connected</span>
      </div>
    </div>

    <div class="stats-grid">
      <div class="stat-box">
        <p class="stat-lbl">Combined Load</p>
        <p class="stat-val" id="valCurrent">0.00 A</p>
        <p class="stat-sub" id="valPower">0 W</p>
      </div>
      <div class="stat-box">
        <p class="stat-lbl">Wi-Fi Signal</p>
        <p class="stat-val" id="valSSID">---</p>
        <p class="stat-sub" id="valRSSI">RSSI: -- dBm</p>
      </div>
    </div>

    <!-- Master Shutdown Control -->
    <div class="card shutdown-card" id="shutdownCard" onclick="toggleShutdown()">
      <div class="shutdown-content">
        <div class="shutdown-icon">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10" stroke-linecap="round" stroke-linejoin="round"/>
          </svg>
        </div>
        <div class="shutdown-text">
          <span class="shutdown-title">Master Shutdown</span>
          <span class="shutdown-desc" id="shutdownDesc">All relays operational</span>
        </div>
      </div>
      <div class="shutdown-badge" id="shutdownBadge">Ready</div>
    </div>

    <div style="height: 1px; background: var(--border); margin-bottom: 20px;"></div>

    <p class="controls-title">System Lights</p>
    
    <div class="relay-grid" id="relayContainer"></div>
  </div>

  <p class="footer">SmartNest-Incubation local control network. Uptime: <span id="valUptime">0s</span></p>
</div>

<script>
  let relayStates = [false, false, false, false, false, false];
  let lockStates = [false, false, false, false, false, false];
  const relayNames = ["Light 1", "Light 2", "Light 3", "Light 4", "Light 5", "Power Socket"];

  const lockIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg>`;
  const unlockIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 9.9-1"></path></svg>`;

  function initUI() {
    const container = document.getElementById('relayContainer');
    container.innerHTML = '';
    for(let i=0; i<6; i++) {
      const card = document.createElement('div');
      card.className = 'relay-card';
      card.id = `relay-${i}`;
      card.onclick = () => toggleRelay(i);
      card.innerHTML = `
        <div class="relay-info">
          <span class="relay-name">${relayNames[i]}</span>
          <span class="relay-status" id="status-${i}">Inactive</span>
        </div>
        <div style="display: flex; align-items: center; gap: 10px;">
          <button class="lock-btn" id="lock-${i}" onclick="toggleLock(event, ${i})"></button>
          <div class="toggle-switch"></div>
        </div>
      `;
      container.appendChild(card);
    }
    updateUI();
  }

  function updateUI() {
    for(let i=0; i<6; i++) {
      const card = document.getElementById(`relay-${i}`);
      const statusText = document.getElementById(`status-${i}`);
      const lockBtn = document.getElementById(`lock-${i}`);
      
      if (lockStates[i]) {
        card.classList.add('locked');
        card.classList.remove('active');
        lockBtn.innerHTML = lockIcon;
        statusText.textContent = 'LOCKED OFF';
      } else {
        card.classList.remove('locked');
        lockBtn.innerHTML = unlockIcon;
        if (relayStates[i]) {
          card.classList.add('active');
          statusText.textContent = 'Active ON';
        } else {
          card.classList.remove('active');
          statusText.textContent = 'Inactive';
        }
      }
    }
  }

  function toggleRelay(index) {
    if (lockStates[index]) return;
    const nextState = !relayStates[index];
    relayStates[index] = nextState;
    updateUI();

    fetch('/api/relay', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ relay: index, state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.success) {
        relayStates[index] = d.state;
        updateUI();
      }
    })
    .catch(() => {
      relayStates[index] = !nextState;
      updateUI();
    });
  }

  function toggleLock(event, index) {
    event.stopPropagation();
    const nextState = !lockStates[index];
    lockStates[index] = nextState;
    updateUI();

    fetch('/api/lock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ relay: index, state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.locked !== undefined) {
        lockStates[index] = d.locked;
        updateUI();
      }
    })
    .catch(() => {
      lockStates[index] = !nextState;
      updateUI();
    });
  }

  let shutdownState = false;

  function toggleShutdown() {
    const nextState = !shutdownState;
    shutdownState = nextState;
    updateShutdownUI();

    fetch('/api/shutdown', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.success) {
        shutdownState = d.shutdown;
        updateShutdownUI();
      }
    })
    .catch(() => {
      shutdownState = !nextState;
      updateShutdownUI();
    });
  }

  function updateShutdownUI() {
    const card = document.getElementById('shutdownCard');
    const desc = document.getElementById('shutdownDesc');
    const badge = document.getElementById('shutdownBadge');
    
    if (shutdownState) {
      card.classList.add('active');
      desc.textContent = 'SYSTEM SHUTDOWN ACTIVE';
      badge.textContent = 'SHUTDOWN';
    } else {
      card.classList.remove('active');
      desc.textContent = 'All relays operational';
      badge.textContent = 'Ready';
    }
  }

  function fetchStatus() {
    fetch('/api/status')
      .then(r => r.json())
      .then(d => {
        relayStates = d.relays;
        lockStates = d.locks || [false, false, false, false, false, false];
        updateUI();
        shutdownState = d.shutdown;
        updateShutdownUI();
        document.getElementById('valCurrent').textContent = d.current.toFixed(2) + ' A';
        document.getElementById('valPower').textContent = Math.round(d.current * 230) + ' W';
        document.getElementById('valSSID').textContent = d.ssid || 'Unknown';
        document.getElementById('valRSSI').textContent = 'RSSI: ' + d.rssi + ' dBm';
        
        let sec = Math.floor(d.uptime / 1000);
        let min = Math.floor(sec / 60);
        let hr = Math.floor(min / 60);
        sec %= 60; min %= 60;
        let uptimeStr = "";
        if(hr > 0) uptimeStr += hr + "h ";
        if(min > 0) uptimeStr += min + "m ";
        uptimeStr += sec + "s";
        document.getElementById('valUptime').textContent = uptimeStr;
      })
      .catch(err => console.error("Error fetching status:", err));
  }

  initUI();
  fetchStatus();
  setInterval(fetchStatus, 1500);
</script>
</body>
</html>
)rawliteral";
