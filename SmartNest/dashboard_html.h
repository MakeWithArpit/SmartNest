#pragma once
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartNest | Premium Dashboard</title>
<style>
  :root {
    --primary: hsl(221, 100%, 45%);
    --primary-light: #3b82f6;
    --primary-gradient: linear-gradient(135deg, #1e40af, #3b82f6);
    --bg-dark: #070b19;
    --card-bg: rgba(13, 20, 44, 0.6);
    --border: rgba(255, 255, 255, 0.08);
    --success: #10b981;
    --danger: #ef4444;
    --warning: #f59e0b;
    --text-light: #f8fafc;
    --text-muted: #94a3b8;
    --font-stack: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  * { margin:0; padding:0; box-sizing:border-box; }
  body {
    font-family: var(--font-stack);
    background-color: var(--bg-dark);
    background-image: radial-gradient(circle at 50% -20%, #1e293b 0%, #070b19 80%);
    color: var(--text-light);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 24px 16px;
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
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    border: 1px solid var(--border);
    border-radius: 20px;
    padding: 24px;
    box-shadow: 0 12px 40px rgba(0,0,0,0.6);
    transition: transform 0.2s ease, border-color 0.2s ease;
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
    gap: 12px;
  }
  .logo-svg {
    width: 36px;
    height: 36px;
  }
  .header-title {
    font-size: 20px;
    font-weight: 800;
    letter-spacing: -0.5px;
    background: linear-gradient(120deg, #f8fafc, #94a3b8);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
  }
  .status-badge {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
    background: rgba(16, 185, 129, 0.1);
    color: var(--success);
    padding: 6px 12px;
    border-radius: 20px;
    border: 1px solid rgba(16, 185, 129, 0.2);
    transition: all 0.3s ease;
  }
  .status-badge.offline {
    background: rgba(239, 68, 68, 0.1);
    color: var(--danger);
    border-color: rgba(239, 68, 68, 0.2);
  }
  .status-badge.offline .status-dot {
    background: var(--danger);
  }
  .status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--success);
    animation: pulse 1.8s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 1; transform: scale(1); }
    50% { opacity: 0.4; transform: scale(0.85); }
  }
  .section-title {
    font-size: 11px;
    font-weight: 800;
    color: var(--text-muted);
    text-transform: uppercase;
    margin-bottom: 12px;
    letter-spacing: 1px;
  }
  .stats-grid-3 {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
    margin-bottom: 16px;
  }
  .stat-box-mini {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 12px 6px;
    text-align: center;
  }
  .stat-val-big {
    font-size: 15px;
    font-weight: 800;
    color: #e2e8f0;
  }
  .details-row {
    background: rgba(255, 255, 255, 0.01);
    border-radius: 10px;
    padding: 10px 14px;
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 8px;
    font-size: 11px;
    color: var(--text-muted);
    border: 1px solid rgba(255, 255, 255, 0.03);
  }
  .details-item span {
    color: #e2e8f0;
    font-weight: 600;
  }
  .slave-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 10px 14px;
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 12px;
    font-size: 13px;
  }
  .slave-info-box {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .slave-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--danger);
  }
  .slave-dot.online {
    background: var(--success);
    box-shadow: 0 0 8px var(--success);
  }
  .slave-rssi {
    font-size: 11px;
    color: var(--text-muted);
  }
  .btn-group {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
    margin-bottom: 10px;
  }
  .cmd-btn {
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid var(--border);
    color: var(--text-light);
    border-radius: 12px;
    padding: 12px 8px;
    font-size: 12px;
    font-weight: 700;
    cursor: pointer;
    transition: all 0.2s ease;
  }
  .cmd-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    border-color: rgba(255, 255, 255, 0.2);
    transform: translateY(-1px);
  }
  .cmd-btn:active {
    transform: translateY(1px);
  }
  .relay-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 12px;
  }
  .relay-card {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    cursor: pointer;
    transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
  }
  .relay-card:hover {
    background: rgba(255, 255, 255, 0.04);
    border-color: rgba(255, 255, 255, 0.15);
  }
  .relay-card.active {
    background: rgba(59, 130, 246, 0.08);
    border-color: rgba(59, 130, 246, 0.3);
  }
  .relay-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .relay-name {
    font-size: 14px;
    font-weight: 700;
    color: #e2e8f0;
  }
  .relay-status {
    font-size: 11px;
    color: var(--text-muted);
  }
  .relay-card.active .relay-status {
    color: var(--primary-light);
    font-weight: 700;
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
  .lock-btn {
    background: rgba(255, 255, 255, 0.04);
    border: 1px solid var(--border);
    color: var(--text-muted);
    border-radius: 10px;
    width: 32px;
    height: 32px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s ease;
  }
  .lock-btn:hover {
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-light);
  }
  .relay-card.locked {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.08) 0%, rgba(220, 38, 38, 0.02) 100%);
    border-color: rgba(239, 68, 68, 0.3);
  }
  .relay-card.locked .relay-status {
    color: var(--danger) !important;
    font-weight: 700;
  }
  .relay-card.locked .toggle-switch {
    background: hsl(223, 14%, 20%) !important;
    opacity: 0.5;
  }
  .relay-card.locked .toggle-switch::after {
    transform: translateX(0) !important;
  }
  .relay-card.locked .lock-btn {
    background: rgba(239, 68, 68, 0.15);
    border-color: rgba(239, 68, 68, 0.4);
    color: var(--danger);
  }
  .lock-btn svg {
    width: 15px;
    height: 15px;
  }
  .shutdown-card {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.06) 0%, rgba(220, 38, 38, 0.02) 100%);
    border: 1px solid rgba(239, 68, 68, 0.2);
    display: flex;
    justify-content: space-between;
    align-items: center;
    cursor: pointer;
    transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
    padding: 16px 20px;
    border-radius: 16px;
  }
  .shutdown-card:hover {
    border-color: rgba(239, 68, 68, 0.4);
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.1) 0%, rgba(220, 38, 38, 0.04) 100%);
  }
  .shutdown-card.active {
    background: linear-gradient(135deg, rgba(239, 68, 68, 0.2) 0%, rgba(220, 38, 38, 0.1) 100%);
    border-color: var(--danger);
    box-shadow: 0 0 20px rgba(239, 68, 68, 0.3);
  }
  .shutdown-content {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .shutdown-icon {
    width: 36px;
    height: 36px;
    border-radius: 50%;
    background: rgba(239, 68, 68, 0.1);
    color: var(--danger);
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.3s ease;
  }
  .shutdown-card.active .shutdown-icon {
    background: var(--danger);
    color: #ffffff;
    transform: rotate(90deg);
  }
  .shutdown-icon svg {
    width: 18px;
    height: 18px;
  }
  .shutdown-badge {
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
    padding: 4px 10px;
    border-radius: 20px;
    border: 1px solid rgba(239, 68, 68, 0.2);
    color: var(--danger);
    background: rgba(239, 68, 68, 0.05);
  }
  .shutdown-card.active .shutdown-badge {
    background: var(--danger);
    color: #ffffff;
    border-color: var(--danger);
  }
  .footer {
    text-align: center;
    font-size: 11px;
    color: var(--text-muted);
    margin-top: 10px;
  }
</style>
</head>
<body>
<div class="container">
  <!-- Header Card -->
  <div class="card">
    <div class="header-row">
      <div class="header-title-box">
        <svg class="logo-svg" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
          <circle cx="50" cy="50" r="45" stroke="#3b82f6" stroke-width="4" stroke-dasharray="8 4" fill="none"/>
          <path d="M50 20 L26 40 L26 70 A 4 4 0 0 0 30 74 L70 74 A 4 4 0 0 0 74 70 L74 40 Z" fill="none" stroke="#3b82f6" stroke-width="5" stroke-linejoin="round"/>
          <circle cx="50" cy="42" r="5" fill="#10b981"/>
          <circle cx="38" cy="58" r="4" fill="#3b82f6"/>
          <circle cx="62" cy="58" r="4" fill="#f59e0b"/>
          <path d="M50 47 L38 58 M50 47 L62 58" stroke="#64748b" stroke-width="2" stroke-linecap="round"/>
        </svg>
        <span class="header-title">SmartNest Premium</span>
      </div>
      <div class="status-badge" id="connectionBadge">
        <span class="status-dot"></span>
        <span id="connectionBadgeText">Connecting</span>
      </div>
    </div>
    
    <div class="stats-grid-3">
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px;">Local Wifi</p>
        <p class="stat-val-big" id="wifiSignalSSID">---</p>
        <p style="font-size: 9px; color: var(--text-muted);" id="wifiSignalRSSI">-- dBm</p>
      </div>
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px;">Active Load</p>
        <p class="stat-val-big" id="combinedAcsCurrent">0.00 A</p>
        <p style="font-size: 9px; color: var(--text-muted);" id="combinedAcsPower">0 W</p>
      </div>
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px;">Master Uptime</p>
        <p class="stat-val-big" id="systemUptime">0s</p>
        <p style="font-size: 9px; color: var(--text-muted);">running</p>
      </div>
    </div>
  </div>

  <!-- Energy Monitor Card -->
  <div class="card">
    <p class="section-title">Energy Monitor</p>
    <div class="stats-grid-3" style="margin-bottom: 12px;">
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px; font-size: 9px;">Daily</p>
        <p class="stat-val-big" id="energyDaily" style="color: var(--primary-light);">0.000 kWh</p>
      </div>
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px; font-size: 9px;">Monthly</p>
        <p class="stat-val-big" id="energyMonthly" style="color: var(--warning);">0.000 kWh</p>
      </div>
      <div class="stat-box-mini">
        <p class="section-title" style="margin-bottom: 2px; font-size: 9px;">Lifetime</p>
        <p class="stat-val-big" id="energyLifetime" style="color: var(--success);">0.00 kWh</p>
      </div>
    </div>
    
    <div class="details-row">
      <div class="details-item">Voltage: <span id="pzemVoltage">0.0 V</span></div>
      <div class="details-item">Power: <span id="pzemPower">0.0 W</span></div>
      <div class="details-item">Combined Load: <span id="pzemCurrent">0.00 A</span></div>
      <div class="details-item">Digital Board: <span id="slaveDigitalCurrent">0.00 A</span></div>
    </div>
  </div>

  <!-- Slave Communication Health Card -->
  <div class="card" style="display: flex; flex-direction: column; gap: 10px;">
    <p class="section-title">Node Network Status</p>
    
    <div class="slave-row">
      <div class="slave-info-box">
        <span class="slave-dot" id="digitalSlaveDot"></span>
        <span>Digital Board Slave</span>
      </div>
      <span class="slave-rssi" id="digitalSlaveRSSI">OFFLINE</span>
    </div>
    
    <div class="slave-row">
      <div class="slave-info-box">
        <span class="slave-dot" id="pzemSlaveDot"></span>
        <span>PZEM Energy Slave</span>
      </div>
      <span class="slave-rssi" id="pzemSlaveRSSI">OFFLINE</span>
    </div>
  </div>

  <!-- Slave Remote Commands Card -->
  <div class="card">
    <p class="section-title">Node Remote Commands</p>
    <div class="btn-group">
      <button class="cmd-btn" onclick="sendSlaveCommand('d1', 'reboot')">Reboot Digital Slave</button>
      <button class="cmd-btn" onclick="sendSlaveCommand('pzem', 'energy_reset')">Reset PZEM Energy</button>
    </div>
    <p id="commandLog" style="color: var(--text-muted); font-size: 11px; margin-top: 6px; text-align: center; min-height: 16px;"></p>
  </div>

  <!-- Master Shutdown Card -->
  <div class="card shutdown-card" id="shutdownCard" onclick="toggleShutdown()">
    <div class="shutdown-content">
      <div class="shutdown-icon">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <path d="M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10" stroke-linecap="round" stroke-linejoin="round"/>
        </svg>
      </div>
      <div class="shutdown-text">
        <span class="relay-name" style="font-size: 15px;">Master Shutdown</span>
        <span class="shutdown-desc" id="shutdownDesc" style="font-size: 11px; color: var(--text-muted);">All relays operational</span>
      </div>
    </div>
    <div class="shutdown-badge" id="shutdownBadge">Ready</div>
  </div>

  <!-- System Lights Card -->
  <div class="card">
    <p class="section-title">System Outlets & Lights</p>
    <div class="relay-grid" id="relayContainer"></div>
  </div>

  <p class="footer">SmartNest Premium IoT Control. Firmware v1.0.0</p>
</div>

<script>
  let relayStates = [false, false, false, false, false, false];
  let lockStates = [false, false, false, false, false, false];
  let shutdownState = false;
  
  let digitalRelayState = false;
  let digitalRelayLocked = false;
  let digitalSwitchState = false;

  const relayNames = ["Light 1", "Light 2", "Light 3", "Light 4", "Light 5", "Power Socket"];

  const lockIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg>`;
  const unlockIcon = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 9.9-1"></path></svg>`;

  function initUI() {
    const container = document.getElementById('relayContainer');
    container.innerHTML = '';
    for(let i=0; i<7; i++) {
      const card = document.createElement('div');
      card.className = 'relay-card';
      card.id = `relay-${i}`;
      card.onclick = () => toggleRelay(i);
      
      let name = (i === 6) ? "Digital Board Relay" : relayNames[i];
      let switchMarkup = (i === 6) 
        ? `<span class="switch-status" id="sw-status-6" style="font-size: 10px; font-weight: 700; padding: 2px 6px; border-radius: 4px; background: rgba(255,255,255,0.05); margin-top: 4px; display: inline-block; width: max-content;">Switch: RELEASED</span>`
        : '';
         
      card.innerHTML = `
        <div class="relay-info" style="display: flex; flex-direction: column;">
          <span class="relay-name">${name}</span>
          <span class="relay-status" id="status-${i}">Inactive</span>
          ${switchMarkup}
        </div>
        <div style="display: flex; align-items: center; gap: 8px;">
          <button class="lock-btn" id="lock-${i}" onclick="toggleLock(event, ${i})"></button>
          <div class="toggle-switch"></div>
        </div>
      `;
      container.appendChild(card);
    }
    updateUI();
  }

  function updateUI() {
    for(let i=0; i<7; i++) {
      const card = document.getElementById(`relay-${i}`);
      const statusText = document.getElementById(`status-${i}`);
      const lockBtn = document.getElementById(`lock-${i}`);
      if (!card || !statusText || !lockBtn) continue;
      
      let isLocked = (i === 6) ? digitalRelayLocked : lockStates[i];
      let isActive = (i === 6) ? digitalRelayState : relayStates[i];
      
      if (isLocked) {
        card.classList.add('locked');
        card.classList.remove('active');
        lockBtn.innerHTML = lockIcon;
        statusText.textContent = 'LOCKED OFF';
      } else {
        card.classList.remove('locked');
        lockBtn.innerHTML = unlockIcon;
        if (isActive) {
          card.classList.add('active');
          statusText.textContent = 'Active ON';
        } else {
          card.classList.remove('active');
          statusText.textContent = 'Inactive';
        }
      }
    }
    
    // Update digital switch state display
    const swStatus6 = document.getElementById('sw-status-6');
    if (swStatus6) {
      if (digitalSwitchState) {
        swStatus6.textContent = 'Switch: PRESSED';
        swStatus6.style.color = 'var(--success)';
        swStatus6.style.background = 'rgba(16, 185, 129, 0.1)';
      } else {
        swStatus6.textContent = 'Switch: RELEASED';
        swStatus6.style.color = 'var(--text-muted)';
        swStatus6.style.background = 'rgba(255, 255, 255, 0.05)';
      }
    }
  }

  function toggleRelay(index) {
    let isLocked = (index === 6) ? digitalRelayLocked : lockStates[index];
    if (isLocked) return;
    
    let currentActive = (index === 6) ? digitalRelayState : relayStates[index];
    const nextState = !currentActive;
    
    if (index === 6) {
      digitalRelayState = nextState;
    } else {
      relayStates[index] = nextState;
    }
    updateUI();

    fetch('/api/relay', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ relay: index, state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.success) {
        if (index === 6) digitalRelayState = d.state;
        else relayStates[index] = d.state;
        updateUI();
      }
    })
    .catch(() => {
      if (index === 6) digitalRelayState = !nextState;
      else relayStates[index] = !nextState;
      updateUI();
    });
  }

  function toggleLock(event, index) {
    event.stopPropagation();
    let currentLocked = (index === 6) ? digitalRelayLocked : lockStates[index];
    const nextState = !currentLocked;
    
    if (index === 6) {
      digitalRelayLocked = nextState;
    } else {
      lockStates[index] = nextState;
    }
    updateUI();

    fetch('/api/lock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ relay: index, state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.locked !== undefined) {
        if (index === 6) digitalRelayLocked = d.locked;
        else lockStates[index] = d.locked;
        updateUI();
      }
    })
    .catch(() => {
      if (index === 6) digitalRelayLocked = !nextState;
      else lockStates[index] = !nextState;
      updateUI();
    });
  }

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
    if (!card || !desc || !badge) return;
    
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

  function sendSlaveCommand(target, command) {
    const log = document.getElementById('commandLog');
    log.textContent = `Sending command ${command} to ${target}...`;
    log.style.color = 'var(--text-light)';
    
    fetch('/api/slave-cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ target: target, cmd: command })
    })
    .then(r => r.json())
    .then(d => {
      if (d.success) {
        log.textContent = `Command successfully sent to ${target}!`;
        log.style.color = 'var(--success)';
      } else {
        log.textContent = `Command failed: ${d.error || 'unknown error'}`;
        log.style.color = 'var(--danger)';
      }
      setTimeout(() => { log.textContent = ''; }, 4000);
    })
    .catch(err => {
      log.textContent = `Error sending command: connection failed`;
      log.style.color = 'var(--danger)';
      setTimeout(() => { log.textContent = ''; }, 4000);
    });
  }

  function applyState(d) {
    // 0-5 relays
    relayStates = d.relays;
    lockStates = d.locks || [false, false, false, false, false, false];
    
    // 6th relay
    digitalRelayState = d.d_relay;
    digitalRelayLocked = d.d_lock;
    digitalSwitchState = d.d_sw;
    
    updateUI();
    
    shutdownState = d.shutdown;
    updateShutdownUI();
    
    // Combined / Active load displays
    document.getElementById('combinedAcsCurrent').textContent = d.current.toFixed(2) + ' A';
    document.getElementById('combinedAcsPower').textContent = Math.round(d.current * (d.v || 230)) + ' W';
    
    // WiFi Signal Displays
    document.getElementById('wifiSignalSSID').textContent = d.ssid || 'Unknown';
    document.getElementById('wifiSignalRSSI').textContent = 'RSSI: ' + d.rssi + ' dBm';
    
    // Energy accumulation displays
    document.getElementById('energyDaily').textContent = d.daily.toFixed(3) + ' kWh';
    document.getElementById('energyMonthly').textContent = d.monthly.toFixed(3) + ' kWh';
    document.getElementById('energyLifetime').textContent = d.lifetime.toFixed(2) + ' kWh';
    
    // Energy detailed sensor parameters
    document.getElementById('pzemVoltage').textContent = d.v.toFixed(1) + ' V';
    document.getElementById('pzemPower').textContent = d.pw.toFixed(1) + ' W';
    document.getElementById('pzemCurrent').textContent = d.load.toFixed(2) + ' A';
    document.getElementById('slaveDigitalCurrent').textContent = d.acs.toFixed(2) + ' A';
    
    // Slaves status network
    const dDot = document.getElementById('digitalSlaveDot');
    const dRSSI = document.getElementById('digitalSlaveRSSI');
    if (d.d_on) {
      dDot.className = 'slave-dot online';
      dRSSI.textContent = d.d_rssi + ' dBm';
    } else {
      dDot.className = 'slave-dot';
      dRSSI.textContent = 'OFFLINE';
    }
    
    const pDot = document.getElementById('pzemSlaveDot');
    const pRSSI = document.getElementById('pzemSlaveRSSI');
    if (d.p_on) {
      pDot.className = 'slave-dot online';
      pRSSI.textContent = d.p_rssi + ' dBm';
    } else {
      pDot.className = 'slave-dot';
      pRSSI.textContent = 'OFFLINE';
    }
    
    // Uptime display formatting
    let sec = Math.floor(d.uptime / 1000);
    let min = Math.floor(sec / 60);
    let hr = Math.floor(min / 60);
    sec %= 60; min %= 60;
    let uptimeStr = "";
    if(hr > 0) uptimeStr += hr + "h ";
    if(min > 0) uptimeStr += min + "m ";
    uptimeStr += sec + "s";
    document.getElementById('systemUptime').textContent = uptimeStr;
  }

  // WebSocket initialization
  const ws = new WebSocket(`ws://${location.hostname}/ws`);
  
  ws.onopen = () => {
    document.getElementById('connectionBadge').classList.remove('offline');
    document.getElementById('connectionBadgeText').textContent = 'Live';
  };
  
  ws.onmessage = (event) => {
    try {
      const state = JSON.parse(event.data);
      applyState(state);
    } catch(err) {
      console.error("Failed to parse WebSocket JSON:", err);
    }
  };
  
  ws.onclose = () => {
    document.getElementById('connectionBadge').classList.add('offline');
    document.getElementById('connectionBadgeText').textContent = 'Disconnected';
    setTimeout(() => location.reload(), 3000);
  };
  
  ws.onerror = (err) => {
    console.error("WebSocket encountered an error:", err);
  };

  initUI();
</script>
</body>
</html>
)rawliteral";
