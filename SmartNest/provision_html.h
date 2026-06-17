#pragma once
static const char PROVISION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartNest-Incubation | Wi-Fi Provisioning</title>
<style>
  :root {
    --primary: hsl(221, 100%, 35%);       /* Deep University Blue */
    --primary-light: hsl(221, 83%, 53%); /* Interactive Soft Blue */
    --secondary: hsl(210, 20%, 98%);     /* Neutral Light Background */
    --success: hsl(162, 72%, 41%);       /* Soft Green Success */
    --error: hsl(354, 76%, 50%);         /* Soft Red Error */
    --text-deep: hsl(224, 71%, 4%);      /* Dark Navy Text */
    --text-muted: hsl(215, 16%, 47%);    /* Slate Grey Description */
    --border-color: hsl(214, 32%, 91%);  /* Low-contrast outline */
    --shadow: 0 10px 25px -5px rgba(0, 0, 0, 0.05), 0 8px 10px -6px rgba(0, 0, 0, 0.05);
    --font-stack: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  }

  * {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
  }

  body {
    font-family: var(--font-stack);
    background-color: var(--secondary);
    color: var(--text-deep);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 20px;
    line-height: 1.5;
  }

  /* Main Container Card */
  .card {
    background: #ffffff;
    border: 1px solid var(--border-color);
    border-radius: 16px;
    padding: 40px 32px;
    max-width: 460px;
    width: 100%;
    box-shadow: var(--shadow);
    display: flex;
    flex-direction: column;
    position: relative;
    overflow: hidden;
  }

  /* Progress Steps Bar */
  .progress-container {
    width: 100%;
    height: 4px;
    background: hsl(210, 20%, 92%);
    position: absolute;
    top: 0;
    left: 0;
  }

  .progress-bar {
    height: 100%;
    width: 16.6%; /* Defaults to 1/6 step */
    background: var(--primary-light);
    transition: width 0.4s cubic-bezier(0.4, 0, 0.2, 1);
  }

  /* Header Branding Area */
  .header-zone {
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    margin-bottom: 28px;
  }

  .logos-row {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 20px;
    margin-bottom: 16px;
    width: 100%;
  }

  .univ-logo-svg {
    width: 50px;
    height: 50px;
  }

  .incub-logo-svg {
    width: 60px;
    height: 60px;
  }

  .product-logo-svg {
    width: 72px;
    height: 72px;
  }

  .brand-title {
    font-size: 24px;
    font-weight: 700;
    color: var(--primary);
    letter-spacing: -0.5px;
  }

  .brand-subtitle {
    font-size: 11px;
    font-weight: 700;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 1.5px;
    margin-top: 2px;
  }

  .divider {
    width: 100%;
    height: 1px;
    background-color: var(--border-color);
    margin: 20px 0;
  }

  /* Content Wizard Screens */
  .screen {
    display: none;
  }

  .screen.active {
    display: block;
    animation: slideIn 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  }

  @keyframes slideIn {
    from { opacity: 0; transform: translateY(8px); }
    to { opacity: 1; transform: translateY(0); }
  }

  /* Screen Title & Text */
  .screen-title {
    font-size: 20px;
    font-weight: 600;
    margin-bottom: 8px;
    color: var(--text-deep);
  }

  .screen-desc {
    font-size: 14px;
    color: var(--text-muted);
    margin-bottom: 24px;
  }

  /* Forms and Inputs */
  .form-group {
    margin-bottom: 20px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }

  .form-label {
    font-size: 11px;
    font-weight: 700;
    color: var(--text-deep);
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }

  .input-wrapper {
    position: relative;
    display: flex;
    align-items: center;
  }

  input[type="text"],
  input[type="password"] {
    width: 100%;
    height: 48px;
    padding: 0 16px;
    padding-right: 50px;
    border: 1px solid var(--border-color);
    border-radius: 8px;
    background: #ffffff;
    color: var(--text-deep);
    font-size: 15px;
    font-family: inherit;
    outline: none;
    transition: all 0.2s ease;
  }

  input:focus {
    border-color: var(--primary-light);
    box-shadow: 0 0 0 3px rgba(33, 112, 228, 0.12);
  }

  .show-pwd-btn {
    position: absolute;
    right: 12px;
    background: none;
    border: none;
    color: var(--text-muted);
    cursor: pointer;
    padding: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    border-radius: 4px;
  }

  .show-pwd-btn:hover {
    color: var(--primary-light);
  }

  /* WiFi Network List */
  .network-list {
    max-height: 240px;
    overflow-y: auto;
    border: 1px solid var(--border-color);
    border-radius: 8px;
    margin-bottom: 24px;
  }

  .network-item {
    width: 100%;
    padding: 14px 16px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: none;
    border: none;
    border-bottom: 1px solid var(--border-color);
    cursor: pointer;
    text-align: left;
    transition: background 0.15s ease;
  }

  .network-item:last-child {
    border-bottom: none;
  }

  .network-item:hover {
    background: hsl(210, 20%, 96%);
  }

  .net-name-box {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .net-icon-svg {
    width: 20px;
    height: 20px;
    fill: var(--text-muted);
  }

  .network-item:hover .net-icon-svg {
    fill: var(--primary-light);
  }

  .net-ssid {
    font-size: 14px;
    font-weight: 600;
    color: var(--text-deep);
  }

  .net-meta {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .net-lock-svg {
    width: 14px;
    height: 14px;
    fill: var(--text-muted);
  }

  .net-signal-badge {
    font-size: 10px;
    font-weight: 700;
    padding: 2px 8px;
    border-radius: 20px;
    text-transform: uppercase;
  }

  .net-signal-badge.excellent { background: rgba(16, 185, 129, 0.1); color: var(--success); }
  .net-signal-badge.good { background: rgba(33, 112, 228, 0.1); color: var(--primary-light); }
  .net-signal-badge.fair { background: rgba(245, 158, 11, 0.1); color: hsl(35, 92%, 50%); }
  .net-signal-badge.weak { background: rgba(239, 68, 68, 0.1); color: var(--error); }

  /* Info / Alert box */
  .info-box {
    background: hsl(210, 20%, 96%);
    border-radius: 8px;
    padding: 12px 16px;
    margin-bottom: 24px;
    font-size: 12px;
    color: var(--text-muted);
    display: flex;
    gap: 10px;
    align-items: flex-start;
  }

  .info-icon-svg {
    width: 16px;
    height: 16px;
    fill: var(--primary-light);
    flex-shrink: 0;
    margin-top: 1px;
  }

  /* Buttons */
  .btn-group {
    display: flex;
    gap: 12px;
  }

  .btn {
    flex: 1;
    height: 48px;
    border-radius: 8px;
    font-size: 14px;
    font-weight: 700;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    transition: all 0.2s ease;
    border: none;
    font-family: inherit;
    text-decoration: none;
  }

  .btn-primary {
    background: var(--primary);
    color: #ffffff;
  }

  .btn-primary:hover {
    background: var(--primary-light);
    transform: translateY(-1px);
  }

  .btn-primary:active {
    transform: translateY(0);
  }

  .btn-secondary {
    background: #ffffff;
    border: 1px solid var(--border-color);
    color: var(--text-deep);
  }

  .btn-secondary:hover {
    background: hsl(210, 20%, 96%);
  }

  .btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
    transform: none !important;
  }

  /* Spinner loader */
  .spinner-wrapper {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 30px 0;
  }

  .spinner {
    width: 48px;
    height: 48px;
    border: 4px solid var(--border-color);
    border-top-color: var(--primary-light);
    border-radius: 50%;
    animation: spin 1s linear infinite;
    margin-bottom: 24px;
  }

  @keyframes spin {
    to { transform: rotate(360deg); }
  }

  /* Success/Error Circles */
  .status-circle {
    width: 64px;
    height: 64px;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    margin: 0 auto 24px auto;
  }

  .status-circle.success {
    background: rgba(16, 185, 129, 0.1);
    border: 3px solid var(--success);
  }

  .status-circle.error {
    background: rgba(239, 68, 68, 0.1);
    border: 3px solid var(--error);
  }

  .status-icon-svg {
    width: 32px;
    height: 32px;
  }

  .status-circle.success .status-icon-svg { fill: var(--success); }
  .status-circle.error .status-icon-svg { fill: var(--error); }

  /* Info Details (Receipt Style) */
  .details-box {
    background: hsl(210, 20%, 96%);
    border: 1px dashed var(--border-color);
    border-radius: 8px;
    padding: 16px;
    margin-bottom: 24px;
    width: 100%;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }

  .details-row {
    display: flex;
    justify-content: space-between;
    font-size: 13px;
  }

  .details-label {
    color: var(--text-muted);
    font-weight: 500;
  }

  .details-val {
    color: var(--text-deep);
    font-weight: 600;
  }

  /* Instructions block */
  .instructions-list {
    text-align: left;
    font-size: 13px;
    color: var(--text-muted);
    margin-bottom: 24px;
    padding-left: 20px;
  }

  .instructions-list li {
    margin-bottom: 8px;
  }

  /* Footer Identity */
  .footer {
    text-align: center;
    font-size: 11px;
    color: var(--text-muted);
    margin-top: 24px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  .footer-links {
    display: flex;
    justify-content: center;
    gap: 12px;
    margin-top: 4px;
  }

  .footer-links a {
    color: var(--primary-light);
    text-decoration: none;
  }

  .footer-links a:hover {
    text-decoration: underline;
  }
</style>
</head>
<body>

<div class="card">
  <!-- Progress Step Bar -->
  <div class="progress-container">
    <div class="progress-bar" id="progressBar"></div>
  </div>

  <!-- Redesigned Welcome Screen -->
  <div class="screen active" id="screen-welcome">
    <div class="header-zone">
      <div class="product-brand-box" style="margin-bottom: 16px;">
        <!-- SmartNest Logo SVG -->
        <svg class="product-logo-svg" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg" style="margin: 0 auto 12px auto; display: block;">
          <circle cx="50" cy="50" r="45" stroke="#0037b0" stroke-width="3" stroke-dasharray="8 4" fill="none"/>
          <circle cx="50" cy="50" r="36" stroke="#2170e4" stroke-width="1" fill="none" opacity="0.5"/>
          <path d="M50 20 L26 40 L26 70 A 4 4 0 0 0 30 74 L70 74 A 4 4 0 0 0 74 70 L74 40 Z" fill="none" stroke="#0037b0" stroke-width="4" stroke-linejoin="round"/>
          <circle cx="50" cy="42" r="5" fill="#10b981"/>
          <circle cx="38" cy="58" r="4" fill="#2170e4"/>
          <circle cx="62" cy="58" r="4" fill="#f59e0b"/>
          <path d="M50 47 L38 58 M50 47 L62 58" stroke="#64748b" stroke-width="2" stroke-linecap="round"/>
        </svg>
        <h1 class="brand-title">SmartNest-Incubation</h1>
        <p class="brand-subtitle">IoT Gateway Provisioning</p>
      </div>
    </div>

    <div class="divider"></div>

    <div style="text-align: center; margin-bottom: 28px;">
      <h2 class="screen-title">Device Setup Wizard</h2>
      <p class="screen-desc" style="margin-bottom: 0;">
        Welcome to the SmartNest-Incubation hub setup experience. Let's securely connect your hardware node to the university incubation center network.
      </p>
    </div>

    <div class="info-box">
      <!-- Info Icon -->
      <svg class="info-icon-svg" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"/></svg>
      <span>This setup portal is served directly from the local ESP32 micro-controller and does not require an active internet connection to load.</span>
    </div>

    <button class="btn btn-primary" style="width: 100%;" onclick="goToStep(2)">Get Started</button>
  </div>

  <!-- Redesigned Wi-Fi Scan Screen -->
  <div class="screen" id="screen-scan">
    <h2 class="screen-title">Available Networks</h2>
    <p class="screen-desc">Select the network you wish to connect your SmartNest node to.</p>

    <!-- Scan Loader -->
    <div class="spinner-wrapper" id="scan-spinner">
      <div class="spinner"></div>
      <p style="font-size: 13px; color: var(--text-muted)">Scanning local 2.4 GHz spectrum...</p>
    </div>

    <!-- Network List Container -->
    <div class="network-list" id="networkList" style="display: none;">
      <!-- Dynamic list elements will be injected here by JS -->
    </div>

    <div class="btn-group">
      <button class="btn btn-secondary" onclick="goToStep(1)">Back</button>
      <button class="btn btn-primary" id="btn-rescan" style="display: none;" onclick="startScan()">Rescan</button>
    </div>
  </div>

  <!-- Redesigned Credentials Screen -->
  <div class="screen" id="screen-credentials">
    <h2 class="screen-title">Network Authentication</h2>
    <p class="screen-desc" id="credentials-network-info">Provide security credentials for the selected network.</p>

    <div class="form-group">
      <label class="form-label">SSID</label>
      <input type="text" id="ssidDisplay" readonly style="color: var(--text-muted); background: hsl(210, 20%, 96%); border-color: var(--border-color);">
    </div>

    <div class="form-group" style="margin-bottom: 24px;">
      <label class="form-label">WPA2 Password</label>
      <div class="input-wrapper">
        <input type="password" id="passwordInput" placeholder="Enter network security key">
        <button class="show-pwd-btn" type="button" onclick="togglePasswordVisibility()">
          <!-- Visibility Icon -->
          <svg id="visibility-icon-svg" style="width: 20px; height: 20px; fill: currentColor;" viewBox="0 0 24 24">
            <path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/>
          </svg>
        </button>
      </div>
    </div>

    <div class="btn-group">
      <button class="btn btn-secondary" onclick="goToStep(2)">Back</button>
      <button class="btn btn-primary" id="btn-connect" onclick="attemptConnection()">Connect</button>
    </div>
  </div>

  <!-- Redesigned Connecting Screen -->
  <div class="screen" id="screen-connecting">
    <div class="spinner-wrapper" style="padding: 40px 0;">
      <div class="spinner"></div>
      <h2 class="screen-title" id="connecting-title">Establishing Connection</h2>
      <p class="screen-desc" id="connecting-subtitle" style="max-width: 320px; margin: 0 auto; text-align: center;">
        Applying credentials and attempting handshake. The ESP32 is connecting to the router. This can take up to 20 seconds.
      </p>
    </div>
  </div>

  <!-- Redesigned Success Screen -->
  <div class="screen" id="screen-success">
    <div class="status-circle success">
      <!-- Check SVG -->
      <svg class="status-icon-svg" viewBox="0 0 24 24"><path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>
    </div>
    
    <div style="text-align: center; margin-bottom: 24px;">
      <h2 class="screen-title">Provisioning Successful</h2>
      <p class="screen-desc">SmartNest-Incubation gateway is now online.</p>
    </div>

    <div class="details-box">
      <div class="details-row">
        <span class="details-label">Connected Network:</span>
        <span class="details-val" id="successSSID">---</span>
      </div>
      <div class="details-row">
        <span class="details-label">Assigned IP Address:</span>
        <span class="details-val" id="successIP" style="color: var(--primary-light);">0.0.0.0</span>
      </div>
      <div class="details-row">
        <span class="details-label">MDNS Hostname:</span>
        <span class="details-val">http://smart-nest.local</span>
      </div>
    </div>

    <div style="text-align: left; margin-bottom: 28px;">
      <h3 style="font-size: 12px; font-weight: 700; text-transform: uppercase; color: var(--text-deep); margin-bottom: 8px; letter-spacing: 0.5px;">Onboarding Steps</h3>
      <ol class="instructions-list">
        <li>Disconnect your device from the temporary <strong>SmartNest_Setup</strong> Wi-Fi access point.</li>
        <li>Reconnect your device/PC back to the same Wi-Fi network listed above (<strong><span id="successSSIDLabel">---</span></strong>).</li>
        <li>Access your browser and load: <a href="http://smart-nest.local" target="_blank" style="color: var(--primary-light); font-weight: 600; text-decoration: none;">http://smart-nest.local</a> or use the IP address shown above to access the local device services.</li>
      </ol>
    </div>

    <div class="info-box" style="margin-bottom: 0;">
      <svg class="info-icon-svg" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"/></svg>
      <span>The ESP32 setup AP will automatically shut down as the chip reboots into normal mode to begin cloud telemetry synchronization.</span>
    </div>
  </div>

  <!-- Redesigned Error Screen -->
  <div class="screen" id="screen-error">
    <div class="status-circle error">
      <!-- Close SVG -->
      <svg class="status-icon-svg" viewBox="0 0 24 24"><path d="M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z"/></svg>
    </div>

    <div style="text-align: center; margin-bottom: 24px;">
      <h2 class="screen-title" style="color: var(--error)">Connection Failed</h2>
      <p class="screen-desc" id="errorMsg">The micro-controller failed to connect to the chosen access point.</p>
    </div>

    <div class="info-box" style="margin-bottom: 28px;">
      <svg class="info-icon-svg" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"/></svg>
      <span>Common failure reasons include incorrect passwords, poor 2.4 GHz signal propagation, or a router MAC filter blockage.</span>
    </div>

    <div class="btn-group">
      <button class="btn btn-secondary" onclick="retryCredentials()">Retry Password</button>
      <button class="btn btn-primary" onclick="goToStep(2)">Rescan</button>
    </div>
  </div>

  <!-- Shared Branding Footer -->
  <div class="footer">
    <p>© 2026 SmartNest-Incubation. Incubation Center.</p>
    <p>Developed under Invertis University directives.</p>
    <div class="footer-links">
      <a href="https://iiif.invertisuniversity.ac.in" target="_blank">Incubation Hub</a>
      <span>•</span>
      <a href="https://www.invertisuniversity.ac.in" target="_blank">Invertis Main</a>
    </div>
  </div>
</div>

<script>
  let currentStep = 1;
  let selectedSSID = "";
  let pollInterval = null;

  // Render network scanning list mock (for offline tests)
  const mockNetworks = [
    { ssid: "Invertis_Campus_Main", rssi: -55, secure: true },
    { ssid: "SmartNest_Admin_2G", rssi: -65, secure: true },
    { ssid: "Research_Lab_EXT", rssi: -80, secure: true },
    { ssid: "IIIF_Guest_Open", rssi: -72, secure: false }
  ];

  function goToStep(step) {
    document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
    
    let activeScreen = 'screen-welcome';
    let progressWidth = '16.6%';
    
    if (step === 1) {
      activeScreen = 'screen-welcome';
      progressWidth = '16.6%';
    } else if (step === 2) {
      activeScreen = 'screen-scan';
      progressWidth = '33.3%';
      startScan();
    } else if (step === 3) {
      activeScreen = 'screen-credentials';
      progressWidth = '50%';
      document.getElementById('ssidDisplay').value = selectedSSID;
    } else if (step === 4) {
      activeScreen = 'screen-connecting';
      progressWidth = '66.6%';
    } else if (step === 5) {
      activeScreen = 'screen-success';
      progressWidth = '100%';
      document.getElementById('successSSID').textContent = selectedSSID;
      document.getElementById('successSSIDLabel').textContent = selectedSSID;
    } else if (step === 6) {
      activeScreen = 'screen-error';
      progressWidth = '100%';
    }
    
    document.getElementById(activeScreen).classList.add('active');
    document.getElementById('progressBar').style.width = progressWidth;
    currentStep = step;
  }

  function startScan() {
    document.getElementById('scan-spinner').style.display = 'flex';
    document.getElementById('networkList').style.display = 'none';
    document.getElementById('btn-rescan').style.display = 'none';

    // Fetch call for runtime (handled gracefully if it fails on local preview)
    fetch('/scan')
      .then(r => r.json())
      .then(data => renderNetworks(data))
      .catch(err => {
        console.warn("API scan failed, using mock data for preview", err);
        setTimeout(() => renderNetworks(mockNetworks), 1000);
      });
  }

  function renderNetworks(networks) {
    document.getElementById('scan-spinner').style.display = 'none';
    const list = document.getElementById('networkList');
    list.innerHTML = '';
    list.style.display = 'block';
    document.getElementById('btn-rescan').style.display = 'block';

    if (networks.length === 0) {
      list.innerHTML = '<div style="padding: 20px; text-align: center; color: var(--text-muted); font-size: 13px;">No 2.4 GHz networks found.</div>';
      return;
    }

    networks.forEach(net => {
      // Determine RSSI strength label
      let rssiClass = 'weak';
      let rssiText = 'Weak';
      if (net.rssi >= -60) {
        rssiClass = 'excellent';
        rssiText = 'Excellent';
      } else if (net.rssi >= -70) {
        rssiClass = 'good';
        rssiText = 'Good';
      } else if (net.rssi >= -82) {
        rssiClass = 'fair';
        rssiText = 'Fair';
      }

      // Check if secure
      const isSecure = net.secure !== false;

      const button = document.createElement('button');
      button.className = 'network-item';
      button.onclick = () => selectNetwork(net.ssid);
      
      // Inline SVGs for network items
      button.innerHTML = `
        <div class="net-name-box">
          <svg class="net-icon-svg" viewBox="0 0 24 24">
            <path d="M12 21l-8.2-10.3c3.7-3.7 8.2-5.7 8.2-5.7s4.5 2 8.2 5.7L12 21zm0-18C7.5 3 3.6 4.9 1 7.2l2.6 3.3c2-1.8 4.9-3.2 8.4-3.2s6.4 1.4 8.4 3.2l2.6-3.3C20.4 4.9 16.5 3 12 3z"/>
          </svg>
          <span class="net-ssid">${net.ssid}</span>
        </div>
        <div class="net-meta">
          ${isSecure ? `
          <svg class="net-lock-svg" viewBox="0 0 24 24">
            <path d="M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z"/>
          </svg>` : ''}
          <span class="net-signal-badge ${rssiClass}">${rssiText}</span>
        </div>
      `;
      list.appendChild(button);
    });
  }

  function selectNetwork(ssid) {
    selectedSSID = ssid;
    goToStep(3);
  }

  function togglePasswordVisibility() {
    const input = document.getElementById('passwordInput');
    const icon = document.getElementById('visibility-icon-svg');
    if (input.type === 'password') {
      input.type = 'text';
      icon.innerHTML = `<path d="M12 7c2.76 0 5 2.24 5 5 0 .65-.13 1.26-.36 1.83l2.92 2.92c1.51-1.26 2.7-2.89 3.44-4.75-1.73-4.39-6-7.5-11-7.5-1.4 0-2.74.25-3.98.7l2.16 2.16C10.74 7.13 11.35 7 12 7zM2 4.27l2.28 2.28.46.46C3.08 8.3 1.78 10.02 1 12c1.73 4.39 6 7.5 11 7.5 1.55 0 3.03-.3 4.38-.84l.42.42L19.73 22 21 20.73 3.27 3 2 4.27zM7.53 9.8l1.55 1.55c-.05.21-.08.43-.08.65 0 1.66 1.34 3 3 3 .22 0 .44-.03.65-.08l1.55 1.55c-.67.43-1.47.68-2.33.68-2.21 0-4-1.79-4-4 0-.86.25-1.66.68-2.33z"/>`;
    } else {
      input.type = 'password';
      icon.innerHTML = `<path d="M12 4.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5zM12 17c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.66 0-3 1.34-3 3s1.34 3 3 3 3-1.34 3-3-1.34-3-3-3z"/>`;
    }
  }

  function attemptConnection() {
    const password = document.getElementById('passwordInput').value;
    if (password.length < 8) {
      alert("WPA2 standard passwords must be at least 8 characters.");
      return;
    }

    goToStep(4);

    // Call connect route
    fetch('/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'ssid=' + encodeURIComponent(selectedSSID) + '&password=' + encodeURIComponent(password)
    })
    .then(r => r.json())
    .then(data => {
      if (data.status === 'connecting') {
        pollStatus();
      }
    })
    .catch(() => {
      // On local preview (no actual ESP32 webserver) simulate connecting
      console.warn("Connection post failed, simulating status check for preview");
      pollStatus();
    });
  }

  function pollStatus() {
    if (pollInterval) clearInterval(pollInterval);
    let attempts = 0;
    
    pollInterval = setInterval(() => {
      attempts++;
      fetch('/connect-status')
        .then(r => r.json())
        .then(data => {
          if (data.status === 'connected') {
            clearInterval(pollInterval);
            document.getElementById('successIP').textContent = data.ip || '192.168.1.120';
            goToStep(5);
          } else if (data.status === 'failed') {
            clearInterval(pollInterval);
            document.getElementById('errorMsg').textContent = `The device was unable to associate with "${selectedSSID}". Please verify the password and try again.`;
            goToStep(6);
          }
        })
        .catch(err => {
          // If we fail on preview, simulate success or failure after 4 attempts
          if (attempts >= 4) {
            clearInterval(pollInterval);
            // Simulate success
            document.getElementById('successIP').textContent = '192.168.1.144';
            goToStep(5);
          }
        });
    }, 2000);
  }

  function retryCredentials() {
    goToStep(3);
  }
</script>

</body>
</html>

)rawliteral";
