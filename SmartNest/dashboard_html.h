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
  .relay-card.active .toggle-switch, .toggle-switch.active {
    background: var(--primary-light);
  }
  .relay-card.active .toggle-switch::after, .toggle-switch.active::after {
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
    <div style="font-size: 11px; color: var(--text-muted); margin-top: -12px; margin-bottom: 16px; font-family: monospace;" id="systemTime">Time: N/A</div>
    
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

  <!-- SD Storage System Card -->
  <div class="card">
    <p class="section-title">SD Storage System</p>
    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px;">
      <span style="font-size: 14px; font-weight: 700;">SD Card Status</span>
      <span class="status-badge" id="sdStatusBadge" style="font-size: 11px; padding: 4px 10px;">Checking...</span>
    </div>
    <div style="margin-bottom: 12px;">
      <div style="display: flex; justify-content: space-between; font-size: 11px; color: var(--text-muted); margin-bottom: 4px;">
        <span>Usage Info</span>
        <span id="sdUsageRatio">0%</span>
      </div>
      <div style="width: 100%; height: 8px; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; border: 1px solid var(--border);">
        <div id="sdProgressBar" style="width: 0%; height: 100%; background: var(--primary-gradient); transition: width 0.3s ease;"></div>
      </div>
    </div>
    <div class="details-row">
      <div class="details-item">Total Space: <span id="sdTotalSpace">0.0 MB</span></div>
      <div class="details-item">Used Space: <span id="sdUsedSpace">0.0 MB</span></div>
    </div>
  </div>

  <!-- SD Log CSV Download Card -->
  <div class="card" id="logDownloadCard">
    <p class="section-title">SD Event Logs</p>
    <div style="display: flex; flex-direction: column; gap: 12px; align-items: center;">
      <button class="cmd-btn" id="downloadCsvBtn" onclick="startCsvDownload()" style="width:100%; font-size:12px; font-weight:800; background:rgba(59,130,246,0.08); border-color:rgba(59,130,246,0.3); color:var(--primary-light); display:flex; align-items:center; justify-content:center; gap:8px; padding: 12px; outline: none;">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width:16px; height:16px; display:inline-block; vertical-align:middle;">
          <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/>
        </svg>
        Download Logs as CSV
      </button>
      <div id="logDownloadProgress" style="display:none; width:100%;">
        <div style="display:flex; justify-content:space-between; font-size:11px; color:var(--text-muted); margin-bottom:4px;">
          <span id="logDownloadStatus">Preparing download...</span>
          <span id="logDownloadPercent">0%</span>
        </div>
        <div style="width: 100%; height: 6px; background: rgba(255,255,255,0.05); border-radius: 3px; overflow: hidden; border: 1px solid var(--border);">
          <div id="logDownloadProgressBar" style="width: 0%; height: 100%; background: var(--primary-gradient); transition: width 0.2s ease;"></div>
        </div>
      </div>
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
        <div style="display:flex;flex-direction:column;">
          <span>PZEM Energy Slave</span>
          <span style="font-size:9px;font-weight:700;" id="pzemHealthLabel">---</span>
        </div>
      </div>
      <span class="slave-rssi" id="pzemSlaveRSSI">OFFLINE</span>
    </div>
    
    <div class="slave-row">
      <div class="slave-info-box">
        <span class="slave-dot" id="mqttStatusDot"></span>
        <span>MQTT Cloud</span>
      </div>
      <span class="slave-rssi" id="mqttStatusLabel">DISABLED</span>
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

  <!-- Master Lock Card -->
  <div class="card" style="border-color: rgba(245, 158, 11, 0.2); background: linear-gradient(135deg, rgba(245, 158, 11, 0.05) 0%, rgba(245, 158, 11, 0.01) 100%);">
    <div style="display: flex; justify-content: space-between; align-items: center;">
      <div style="display: flex; align-items: center; gap: 12px;">
        <div style="width: 36px; height: 36px; border-radius: 50%; background: rgba(245, 158, 11, 0.1); color: var(--warning); display: flex; align-items: center; justify-content: center;" id="masterLockIcon">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="width: 18px; height: 18px;">
            <rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path>
          </svg>
        </div>
        <div>
          <span class="relay-name" style="font-size: 15px;">Global Master Lock</span>
          <p style="font-size: 11px; color: var(--text-muted);" id="masterLockDesc">System is unlocked</p>
        </div>
      </div>
      <div class="toggle-switch" id="masterLockToggleBtn" onclick="toggleMasterLock()" style="cursor: pointer;"></div>
    </div>
  </div>

  <!-- System Lights Card -->
  <div class="card">
    <p class="section-title">System Outlets & Lights</p>
    <div class="relay-grid" id="relayContainer"></div>
  </div>

  <!-- MQTT Configuration Card -->
  <div class="card" id="mqttConfigCard">
    <p class="section-title">MQTT Configuration</p>
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px;">
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Broker Host</label>
        <input type="text" id="mqttBroker" placeholder="broker.hivemq.com" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Port</label>
        <input type="number" id="mqttPort" placeholder="1883" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Client ID</label>
        <input type="text" id="mqttClientId" placeholder="SmartNest_001" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Base Topic</label>
        <input type="text" id="mqttBaseTopic" placeholder="smartnest" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Username</label>
        <input type="text" id="mqttUsername" placeholder="(optional)" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Password</label>
        <input type="password" id="mqttPassword" placeholder="(optional)" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div>
        <label style="font-size:10px;color:var(--text-muted);display:block;margin-bottom:2px;">Keep Alive (s)</label>
        <input type="number" id="mqttKeepAlive" placeholder="60" style="width:100%;background:rgba(255,255,255,0.03);border:1px solid var(--border);border-radius:8px;color:var(--text-light);padding:8px;font-size:12px;outline:none;">
      </div>
      <div style="display:flex;align-items:flex-end;">
        <label style="font-size:11px;color:var(--text-muted);display:flex;align-items:center;gap:6px;cursor:pointer;">
          <input type="checkbox" id="mqttEnabled" style="accent-color:var(--primary-light);">
          MQTT Enabled
        </label>
      </div>
    </div>
    <div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;">
      <button class="cmd-btn" onclick="saveMqttConfig()" style="font-size:11px;background:rgba(16,185,129,0.1);border-color:rgba(16,185,129,0.3);color:var(--success);">Save Config</button>
      <button class="cmd-btn" onclick="testMqttConnection()" style="font-size:11px;">Test Connect</button>
      <button class="cmd-btn" onclick="resetMqttDefaults()" style="font-size:11px;color:var(--warning);border-color:rgba(245,158,11,0.3);">Reset Defaults</button>
    </div>
    <p id="mqttConfigMsg" style="color:var(--text-muted);font-size:11px;margin-top:8px;text-align:center;min-height:16px;"></p>
  </div>

  <!-- System Maintenance Card -->
  <div class="card" style="border-color: rgba(239, 68, 68, 0.15);">
    <p class="section-title">System Maintenance</p>
    <div class="btn-group" style="margin-bottom:8px;">
      <button class="cmd-btn" onclick="modularReset('wifi')" style="font-size:11px;">Reset WiFi</button>
      <button class="cmd-btn" onclick="modularReset('mqtt')" style="font-size:11px;">Reset MQTT</button>
      <button class="cmd-btn" onclick="modularReset('energy')" style="font-size:11px;">Reset Energy</button>
      <button class="cmd-btn" onclick="modularReset('logs')" style="font-size:11px;">Clear SD Logs</button>
    </div>
    <button class="cmd-btn" id="factoryResetBtn" onclick="triggerFactoryReset()" style="width:100%; background: rgba(239, 68, 68, 0.1); border-color: rgba(239, 68, 68, 0.2); color: var(--danger); font-weight: 800;">
      FULL FACTORY RESET
    </button>
    <p id="resetMsg" style="color:var(--text-muted);font-size:11px;margin-top:8px;text-align:center;min-height:16px;"></p>
  </div>

  <p class="footer">SmartNest Premium IoT Control. Firmware v1.0.0</p>
</div>

<script>
  let relayStates = [false, false, false, false, false, false];
  let lockStates = [false, false, false, false, false, false];
  let shutdownState = false;
  let masterLock = false;
  
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
      
      if (masterLock) {
        card.classList.add('locked');
        card.classList.remove('active');
        lockBtn.innerHTML = lockIcon;
        statusText.textContent = 'MASTER LOCKED';
        lockBtn.style.opacity = '0.5';
      } else if (isLocked) {
        card.classList.add('locked');
        card.classList.remove('active');
        lockBtn.innerHTML = lockIcon;
        statusText.textContent = 'LOCKED OFF';
        lockBtn.style.opacity = '1';
      } else {
        card.classList.remove('locked');
        lockBtn.innerHTML = unlockIcon;
        lockBtn.style.opacity = '1';
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
    if (!d) return;

    // 0-5 relays
    if (Array.isArray(d.relays)) relayStates = d.relays;
    if (Array.isArray(d.locks)) lockStates = d.locks;
    
    // 6th relay
    if (typeof d.d_relay === "boolean") digitalRelayState = d.d_relay;
    if (typeof d.d_lock === "boolean") digitalRelayLocked = d.d_lock;
    if (typeof d.d_sw === "boolean") digitalSwitchState = d.d_sw;
    
    // masterLock
    if (typeof d.m_lock === "boolean") {
      masterLock = d.m_lock;
      updateMasterLockUI();
    }

    updateUI();
    
    if (typeof d.shutdown === "boolean") {
      shutdownState = d.shutdown;
      updateShutdownUI();
    }

    // Time display
    const timeRow = document.getElementById('systemTime');
    if (timeRow && d.time) {
      timeRow.textContent = 'Controller Time: ' + d.time;
    }
    
    // Combined / Active load displays
    if (typeof d.current === "number") {
      document.getElementById('combinedAcsCurrent').textContent = d.current.toFixed(2) + ' A';
      document.getElementById('combinedAcsPower').textContent = Math.round(d.current * (d.v || 230)) + ' W';
    }
    
    // WiFi Signal Displays
    if (d.ssid !== undefined) document.getElementById('wifiSignalSSID').textContent = d.ssid || 'Unknown';
    if (typeof d.rssi === "number") document.getElementById('wifiSignalRSSI').textContent = 'RSSI: ' + d.rssi + ' dBm';
    
    // Energy accumulation displays
    if (typeof d.daily === "number") document.getElementById('energyDaily').textContent = d.daily.toFixed(3) + ' kWh';
    if (typeof d.monthly === "number") document.getElementById('energyMonthly').textContent = d.monthly.toFixed(3) + ' kWh';
    if (typeof d.lifetime === "number") document.getElementById('energyLifetime').textContent = d.lifetime.toFixed(2) + ' kWh';
    
    // Energy detailed sensor parameters
    if (typeof d.v === "number") document.getElementById('pzemVoltage').textContent = d.v.toFixed(1) + ' V';
    if (typeof d.pw === "number") document.getElementById('pzemPower').textContent = d.pw.toFixed(1) + ' W';
    if (typeof d.load === "number") document.getElementById('pzemCurrent').textContent = d.load.toFixed(2) + ' A';
    if (typeof d.acs === "number") document.getElementById('slaveDigitalCurrent').textContent = d.acs.toFixed(2) + ' A';
    
    // SD Card Displays
    const sdBadge = document.getElementById('sdStatusBadge');
    const sdTotal = document.getElementById('sdTotalSpace');
    const sdUsed = document.getElementById('sdUsedSpace');
    const sdProgress = document.getElementById('sdProgressBar');
    const sdRatio = document.getElementById('sdUsageRatio');
    
    if (sdBadge && sdTotal && sdUsed && sdProgress && sdRatio && typeof d.sd_ok === "boolean") {
      if (d.sd_ok) {
        sdBadge.textContent = 'ONLINE';
        sdBadge.className = 'status-badge';
        sdBadge.style.color = 'var(--success)';
        sdBadge.style.borderColor = 'rgba(16, 185, 129, 0.2)';
        sdBadge.style.background = 'rgba(16, 185, 129, 0.1)';
        
        const totalMB = d.sd_total / (1024 * 1024);
        const usedMB = d.sd_used / (1024 * 1024);
        sdTotal.textContent = totalMB.toFixed(1) + ' MB';
        sdUsed.textContent = usedMB.toFixed(1) + ' MB';
        
        const percent = totalMB > 0 ? (usedMB / totalMB) * 100 : 0;
        sdRatio.textContent = percent.toFixed(1) + '%';
        sdProgress.style.width = percent.toFixed(1) + '%';
      } else {
        sdBadge.textContent = 'OFFLINE';
        sdBadge.className = 'status-badge offline';
        sdBadge.style.color = 'var(--danger)';
        sdBadge.style.borderColor = 'rgba(239, 68, 68, 0.2)';
        sdBadge.style.background = 'rgba(239, 68, 68, 0.1)';
        
        sdTotal.textContent = '---';
        sdUsed.textContent = '---';
        sdRatio.textContent = '0%';
        sdProgress.style.width = '0%';
      }
    }

    // Slaves status network
    const dDot = document.getElementById('digitalSlaveDot');
    const dRSSI = document.getElementById('digitalSlaveRSSI');
    if (dDot && dRSSI && typeof d.d_on === "boolean") {
      if (d.d_on) {
        dDot.className = 'slave-dot online';
        dRSSI.textContent = d.d_rssi + ' dBm';
      } else {
        dDot.className = 'slave-dot';
        dRSSI.textContent = 'OFFLINE';
      }
    }
    
    const pDot = document.getElementById('pzemSlaveDot');
    const pRSSI = document.getElementById('pzemSlaveRSSI');
    const pHealth = document.getElementById('pzemHealthLabel');
    if (pDot && pRSSI && pHealth && typeof d.p_on === "boolean") {
      if (d.p_on) {
        pDot.className = 'slave-dot online';
        pRSSI.textContent = d.p_rssi + ' dBm';
        if (d.p_health) {
          pHealth.textContent = 'HEALTHY';
          pHealth.style.color = 'var(--success)';
        } else {
          pHealth.textContent = 'SENSOR ERROR';
          pHealth.style.color = 'var(--warning)';
        }
      } else {
        pDot.className = 'slave-dot';
        pRSSI.textContent = 'OFFLINE';
        pHealth.textContent = 'OFFLINE';
        pHealth.style.color = 'var(--danger)';
      }
    }
    
    // MQTT Cloud status
    const mqttDot = document.getElementById('mqttStatusDot');
    const mqttLabel = document.getElementById('mqttStatusLabel');
    if (mqttDot && mqttLabel && typeof d.mqtt_status === "number") {
      const mqttStNames = ['DISABLED','CONNECTING','CONNECTED','ERROR'];
      const mqttSt = d.mqtt_status;
      mqttLabel.textContent = mqttStNames[mqttSt] || 'UNKNOWN';
      if (mqttSt === 2) {
        mqttDot.className = 'slave-dot online';
        mqttLabel.style.color = 'var(--success)';
      } else if (mqttSt === 1) {
        mqttDot.className = 'slave-dot';
        mqttDot.style.background = 'var(--warning)';
        mqttLabel.style.color = 'var(--warning)';
      } else {
        mqttDot.className = 'slave-dot';
        mqttLabel.style.color = 'var(--text-muted)';
      }
    }
    
    // Uptime display formatting
    if (typeof d.uptime === "number") {
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
  }

  // ─── CSV Download State ───
  let csvDownloading = false;
  let csvFileToDownload = "";
  let csvChunkIndex = 0;
  let csvTotalRecords = 0;
  let csvCollectedRows = [];

  // ─── Fetch initial state on page load ───
  function fetchInitialState() {
    fetch('/api/status')
      .then(r => r.json())
      .then(d => { applyState(d); })
      .catch(err => console.error("Initial status fetch failed:", err));
  }

  // ─── CSV Download Functions ───
  function startCsvDownload() {
    if (csvDownloading) return;
    const btn = document.getElementById('downloadCsvBtn');
    const progress = document.getElementById('logDownloadProgress');
    const status = document.getElementById('logDownloadStatus');
    const percent = document.getElementById('logDownloadPercent');
    const bar = document.getElementById('logDownloadProgressBar');

    btn.disabled = true;
    btn.style.opacity = '0.5';
    progress.style.display = 'block';
    status.textContent = 'Requesting file list...';
    percent.textContent = '0%';
    bar.style.width = '0%';
    csvDownloading = true;
    csvCollectedRows = [];
    csvChunkIndex = 0;
    csvTotalRecords = 0;
    csvFileToDownload = "";

    fetch('/api/logs/list')
      .then(r => r.json())
      .catch(err => {
        status.textContent = 'Failed to request file list';
        status.style.color = 'var(--danger)';
        resetCsvDownloadUI();
      });
  }

  function handleCsvFileList(files) {
    const status = document.getElementById('logDownloadStatus');
    if (!csvDownloading) return;
    if (!files || files.length === 0) {
      status.textContent = 'No log files found on SD card';
      status.style.color = 'var(--warning)';
      resetCsvDownloadUI();
      return;
    }
    // Pick the latest .dat file (last in sorted list)
    const datFiles = files.filter(f => f.endsWith('.dat'));
    if (datFiles.length === 0) {
      status.textContent = 'No .dat log files found';
      status.style.color = 'var(--warning)';
      resetCsvDownloadUI();
      return;
    }
    csvFileToDownload = datFiles[datFiles.length - 1];
    csvChunkIndex = 0;
    status.textContent = 'Downloading ' + csvFileToDownload + '...';
    requestDownloadChunk();
  }

  function requestDownloadChunk() {
    fetch('/api/logs/view?file=' + encodeURIComponent(csvFileToDownload) + '&chunk=' + csvChunkIndex)
      .then(r => r.json())
      .catch(err => {
        const status = document.getElementById('logDownloadStatus');
        status.textContent = 'Chunk request failed';
        status.style.color = 'var(--danger)';
        resetCsvDownloadUI();
      });
  }

  function handleCsvChunkReceived(d) {
    if (!csvDownloading) return;
    if (d.file !== csvFileToDownload) return;

    const status = document.getElementById('logDownloadStatus');
    const percent = document.getElementById('logDownloadPercent');
    const bar = document.getElementById('logDownloadProgressBar');

    const recs = d.recs || [];
    const total = d.total || 0;
    csvTotalRecords = total;

    recs.forEach(r => {
      csvCollectedRows.push(r);
    });

    const totalChunks = Math.max(1, Math.ceil(total / 10));
    const pct = Math.min(100, Math.round(((csvChunkIndex + 1) / totalChunks) * 100));
    percent.textContent = pct + '%';
    bar.style.width = pct + '%';
    status.textContent = 'Chunk ' + (csvChunkIndex + 1) + ' of ' + totalChunks + '...';

    if (recs.length > 0 && (csvChunkIndex + 1) < totalChunks) {
      csvChunkIndex++;
      setTimeout(requestDownloadChunk, 100);
    } else {
      generateAndDownloadCsv();
    }
  }

  function generateAndDownloadCsv() {
    const status = document.getElementById('logDownloadStatus');
    const percent = document.getElementById('logDownloadPercent');
    const bar = document.getElementById('logDownloadProgressBar');

    if (csvCollectedRows.length === 0) {
      status.textContent = 'No records to export';
      status.style.color = 'var(--warning)';
      resetCsvDownloadUI();
      return;
    }

    status.textContent = 'Generating CSV...';
    percent.textContent = '100%';
    bar.style.width = '100%';

    let csv = 'Timestamp,Voltage (V),Load Current (A),PZEM Current (A),Power (VA)\n';
    csvCollectedRows.forEach(r => {
      let ts = r.epoch ? new Date(r.epoch * 1000).toISOString() : 'N/A';
      csv += ts + ',' + (r.v||0).toFixed(1) + ',' + (r.load||0).toFixed(2) + ',' + (r.pi||0).toFixed(3) + ',' + (r.pva||0).toFixed(1) + '\n';
    });

    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = csvFileToDownload.replace('.dat', '') + '_log.csv';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    status.textContent = 'Download complete! (' + csvCollectedRows.length + ' records)';
    status.style.color = 'var(--success)';
    resetCsvDownloadUI();
  }

  function resetCsvDownloadUI() {
    csvDownloading = false;
    const btn = document.getElementById('downloadCsvBtn');
    if (btn) {
      btn.disabled = false;
      btn.style.opacity = '1';
    }
    setTimeout(() => {
      const progress = document.getElementById('logDownloadProgress');
      if (progress) progress.style.display = 'none';
    }, 5000);
  }

  // ─── WebSocket initialization ───
  const ws = new WebSocket(`ws://${location.hostname}/ws`);
  
  ws.onopen = () => {
    document.getElementById('connectionBadge').classList.remove('offline');
    document.getElementById('connectionBadgeText').textContent = 'Live';
  };
  
  ws.onmessage = (event) => {
    try {
      const d = JSON.parse(event.data);
      if (d.t === "files_res") {
        handleCsvFileList(d.files);
      } else if (d.t === "read_res") {
        handleCsvChunkReceived(d);
      } else {
        applyState(d);
      }
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

  function toggleMasterLock() {
    const nextState = !masterLock;
    masterLock = nextState;
    updateMasterLockUI();

    fetch('/api/master-lock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ state: nextState })
    })
    .then(r => r.json())
    .then(d => {
      if(d.success) {
        masterLock = d.state;
        updateMasterLockUI();
      }
    })
    .catch(() => {
      masterLock = !nextState;
      updateMasterLockUI();
    });
  }

  function updateMasterLockUI() {
    const btn = document.getElementById('masterLockToggleBtn');
    const desc = document.getElementById('masterLockDesc');
    const icon = document.getElementById('masterLockIcon');
    if (!btn || !desc || !icon) return;

    if (masterLock) {
      btn.classList.add('active');
      desc.textContent = 'SYSTEM LOCKED (Outlets locked OFF)';
      desc.style.color = 'var(--warning)';
      icon.style.background = 'rgba(245, 158, 11, 0.2)';
    } else {
      btn.classList.remove('active');
      desc.textContent = 'System is unlocked';
      desc.style.color = 'var(--text-muted)';
      icon.style.background = 'rgba(245, 158, 11, 0.1)';
    }
  }

  function triggerFactoryReset() {
    document.getElementById('confirmModal').style.display = 'flex';
  }
  function closeResetModal() {
    document.getElementById('confirmModal').style.display = 'none';
  }
  function executeFactoryReset() {
    closeResetModal();
    const btn = document.getElementById('factoryResetBtn');
    btn.disabled = true;
    btn.textContent = 'RESETTING SYSTEM...';
    
    fetch('/api/factory-reset', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      alert("Factory reset initiated. The device will reboot into provisioning AP 'SmartNest'. Please wait...");
      setTimeout(() => { location.reload(); }, 5000);
    })
    .catch(() => {
      alert("Reset request sent. Device is rebooting...");
      setTimeout(() => { location.reload(); }, 5000);
    });
  }

  function loadMqttConfigUI() {
    const msg = document.getElementById('mqttConfigMsg');
    fetch('/api/mqtt/config')
      .then(r => r.json())
      .then(d => {
        document.getElementById('mqttBroker').value = d.broker || '';
        document.getElementById('mqttPort').value = d.port || 1883;
        document.getElementById('mqttClientId').value = d.clientId || '';
        document.getElementById('mqttBaseTopic').value = d.baseTopic || '';
        document.getElementById('mqttUsername').value = d.username || '';
        document.getElementById('mqttPassword').value = d.password || '';
        document.getElementById('mqttKeepAlive').value = d.keepAlive || 60;
        document.getElementById('mqttEnabled').checked = d.enabled;
      })
      .catch(err => {
        msg.textContent = 'Failed to load MQTT configuration';
        msg.style.color = 'var(--danger)';
      });
  }

  function saveMqttConfig() {
    const msg = document.getElementById('mqttConfigMsg');
    msg.textContent = 'Saving configuration...';
    msg.style.color = 'var(--text-light)';
    
    const body = {
      enabled: document.getElementById('mqttEnabled').checked,
      broker: document.getElementById('mqttBroker').value,
      port: parseInt(document.getElementById('mqttPort').value) || 1883,
      clientId: document.getElementById('mqttClientId').value,
      baseTopic: document.getElementById('mqttBaseTopic').value,
      username: document.getElementById('mqttUsername').value,
      password: document.getElementById('mqttPassword').value,
      keepAlive: parseInt(document.getElementById('mqttKeepAlive').value) || 60
    };

    fetch('/api/mqtt/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    })
    .then(r => r.json())
    .then(d => {
      if (d.success) {
        msg.textContent = d.msg || 'Configuration saved successfully!';
        msg.style.color = 'var(--success)';
      } else {
        msg.textContent = 'Save failed: ' + (d.error || 'unknown error');
        msg.style.color = 'var(--danger)';
      }
      setTimeout(() => { msg.textContent = ''; }, 4000);
    })
    .catch(err => {
      msg.textContent = 'Failed to save configuration';
      msg.style.color = 'var(--danger)';
      setTimeout(() => { msg.textContent = ''; }, 4000);
    });
  }

  function testMqttConnection() {
    const msg = document.getElementById('mqttConfigMsg');
    msg.textContent = 'Testing connection...';
    msg.style.color = 'var(--text-light)';
    
    fetch('/api/mqtt/test', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      msg.textContent = d.msg;
      msg.style.color = d.success ? 'var(--success)' : 'var(--danger)';
      setTimeout(() => { msg.textContent = ''; }, 5000);
    })
    .catch(err => {
      msg.textContent = 'Test connection request failed';
      msg.style.color = 'var(--danger)';
      setTimeout(() => { msg.textContent = ''; }, 5000);
    });
  }

  function resetMqttDefaults() {
    const msg = document.getElementById('mqttConfigMsg');
    msg.textContent = 'Resetting defaults...';
    msg.style.color = 'var(--warning)';
    
    fetch('/api/mqtt/reset-default', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      msg.textContent = d.msg;
      msg.style.color = 'var(--success)';
      loadMqttConfigUI();
      setTimeout(() => { msg.textContent = ''; }, 4000);
    })
    .catch(err => {
      msg.textContent = 'Reset request failed';
      msg.style.color = 'var(--danger)';
      setTimeout(() => { msg.textContent = ''; }, 4000);
    });
  }

  function modularReset(type) {
    const msg = document.getElementById('resetMsg');
    const actions = {
      wifi: 'WiFi credentials clearing...',
      mqtt: 'MQTT config resetting...',
      energy: 'Energy counter resetting...',
      logs: 'SD logs clearing...'
    };
    msg.textContent = actions[type] || 'Resetting...';
    msg.style.color = 'var(--warning)';
    
    fetch(`/api/reset/${type}`, { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      msg.textContent = d.msg;
      msg.style.color = 'var(--success)';
      if (type === 'mqtt') {
        loadMqttConfigUI();
      }
      setTimeout(() => { msg.textContent = ''; }, 5000);
    })
    .catch(err => {
      msg.textContent = 'Reset request failed';
      msg.style.color = 'var(--danger)';
      setTimeout(() => { msg.textContent = ''; }, 5000);
    });
  }

  // ─── Initialization ───
  document.addEventListener('DOMContentLoaded', () => {
    setTimeout(fetchInitialState, 500);
    setTimeout(loadMqttConfigUI, 1500);
  });

  initUI();
</script>

<!-- Confirm Modal -->
<div id="confirmModal" style="display:none; position:fixed; top:0; left:0; width:100%; height:100%; background: rgba(0,0,0,0.8); backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px); z-index: 1000; align-items:center; justify-content:center; padding: 20px;">
  <div class="card" style="max-width: 400px; border-color: var(--danger); background: var(--bg-dark);">
    <p class="section-title" style="color: var(--danger); font-size:14px; margin-bottom: 12px;">Factory Reset Confirmation</p>
    <p style="font-size:12px; color: var(--text-light); margin-bottom: 20px; line-height: 1.6;">
      WARNING: This action will permanently erase all Wi-Fi credentials on the SmartNest controller and delete all binary log records (/state.bin and /sync.bin) from the SD card. Energy accumulation statistics will be reset to zero. This cannot be undone!
    </p>
    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:12px;">
      <button class="cmd-btn" onclick="closeResetModal()">Cancel</button>
      <button class="cmd-btn" onclick="executeFactoryReset()" style="background: var(--danger); border-color: var(--danger); color: white;">Confirm Reset</button>
    </div>
  </div>
</div>
</body>
</html>
)rawliteral";
