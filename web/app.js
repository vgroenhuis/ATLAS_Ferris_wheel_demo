const FALLBACK_DATA = {
  channels: {
    ch2: { rope: 0 }, // N
    ch3: { rope: 0 }, // S
    ch4: { rope: 0 }, // E
    ch5: { rope: 0 }  // W
  },
  forces: {
    axial: 0,
    yaw: 0,
    pitch: 0
  }
};

// ---------------- GRAPH DATA ----------------
const maxPoints = 200;                    // 20 s of history at 10 points a second
const labels = [];

const forceData = [];
const yawData = [];
const pitchData = [];


// ---------------- THREE.JS SETUP ----------------
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x1B192E);

const camera = new THREE.PerspectiveCamera(
  60,
  window.innerWidth / window.innerHeight,
  0.1,
  100
);
camera.position.set(0, -2, 5);
camera.lookAt(0, 0, 0);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(window.innerWidth, window.innerHeight);
document.body.appendChild(renderer.domElement);

// Lighting
scene.add(new THREE.AmbientLight(0xffffff, 0.4));
const dirLight = new THREE.DirectionalLight(0xffffff, 0.8);
dirLight.position.set(5, 10, 5);
scene.add(dirLight);

// ---------------- DISK ----------------
const diskGeometry = new THREE.CircleGeometry(1, 64);
const diskMaterial = new THREE.MeshStandardMaterial({
  color: 0x44aaee,
  side: THREE.DoubleSide,
  roughness: 0.5,
  metalness: 0.2
});
const disk = new THREE.Mesh(diskGeometry, diskMaterial);
disk.rotation.x = -Math.PI / 2;
scene.add(disk);

// Normal line
const normalGeometry = new THREE.BufferGeometry().setFromPoints([
  new THREE.Vector3(0, 0, 0),
  new THREE.Vector3(0, 0, 1.5)
]);
const normalLine = new THREE.Line(
  normalGeometry,
  new THREE.LineBasicMaterial({ color: 0xffff00 })
);
disk.add(normalLine);

// ---------------- GAUGES ----------------
const levelCanvas = document.getElementById("levelCanvas");
const levelCtx = levelCanvas.getContext("2d");
const lc = levelCanvas.width / 2;
const lr = lc - 5;

function drawWaterLevel(rotX, rotY) {
  levelCtx.clearRect(0, 0, levelCanvas.width, levelCanvas.height);

  levelCtx.beginPath();
  levelCtx.arc(lc, lc, lr, 0, Math.PI * 2);
  levelCtx.strokeStyle = "#44aaee";
  levelCtx.lineWidth = 3;
  levelCtx.stroke();

  const maxTilt = 0.3;
  const dx = THREE.MathUtils.clamp(rotY / maxTilt, -1, 1) * lr * 0.6;
  const dy = THREE.MathUtils.clamp(-rotX / maxTilt, -1, 1) * lr * 0.6;

  levelCtx.beginPath();
  levelCtx.arc(lc + dx, lc + dy, 6, 0, Math.PI * 2);
  levelCtx.fillStyle = "#ff4444";
  levelCtx.fill();
}

// Depth meter
const depthCanvas = document.getElementById("depthCanvas");
const depthCtx = depthCanvas.getContext("2d");

function drawDepthMeter(avg) {
  const h = depthCanvas.height;
  const w = depthCanvas.width;
  const maxDev = 5;

  let y = h / 2 - (avg / maxDev) * (h / 2);
  y = Math.min(h, Math.max(0, y));

  depthCtx.clearRect(0, 0, w, h);
  depthCtx.fillStyle = "#222";
  depthCtx.fillRect(0, 0, w, h);

  depthCtx.fillStyle = "#44aaee";
  if (y < h / 2) depthCtx.fillRect(0, y, w, h / 2 - y);
  else depthCtx.fillRect(0, h / 2, w, y - h / 2);

  depthCtx.strokeStyle = "#fff";
  depthCtx.beginPath();
  depthCtx.moveTo(0, h / 2);
  depthCtx.lineTo(w, h / 2);
  depthCtx.stroke();
}

// ---------------- CHARTS ----------------
const forceCtx = document.getElementById("forceChart").getContext("2d");
const torqueCtx = document.getElementById("torqueChart").getContext("2d");

const forceChart = new Chart(forceCtx, {
  type: "line",
  data: {
    labels,
    datasets: [{
      label: "Axial Force (N)",
      data: forceData,
      borderColor: "#44aaee",
      tension: 0.2
    }]
  },
  options: {
    animation: false,
    responsive: true,
    scales: {
      x: { display: false },
      y: { beginAtZero: true }
    }
  }
});

const torqueChart = new Chart(torqueCtx, {
  type: "line",
  data: {
    labels,
    datasets: [
      {
        label: "Yaw (Nm)",
        data: yawData,
        borderColor: "#ff4444",
        tension: 0.2
      },
      {
        label: "Pitch (Nm)",
        data: pitchData,
        borderColor: "#44ff44",
        tension: 0.2
      }
    ]
  },
  options: {
    animation: false,
    responsive: true,
    scales: {
      x: { display: false },
      y: { beginAtZero: true }
    }
  }
});

// ---------------- UPDATE LOGIC ----------------
function updateDisk(N, S, E, W) {
  const rotX = (S - N) * 0.01;
  const rotY = (E - W) * 0.01;
  const avg = (N + S + E + W) / 4;

  disk.position.y = avg * 0.01;
  disk.rotation.x = rotX;
  disk.rotation.y = rotY;

  drawWaterLevel(rotX, rotY);
  drawDepthMeter(avg);
}

// ---------------- ZERO BUTTON ----------------
// Makes the current position the zero point (POST /api/zero). Meant for an unloaded sensor.
const zeroNote = document.getElementById("zeroNote");
document.getElementById("zeroButton").addEventListener("click", async () => {
  try {
    const r = await fetch("/api/zero", { method: "POST" });
    zeroNote.textContent = r.ok ? "Zeroed" : "Needs all 4 sensors";
  } catch {
    zeroNote.textContent = "No connection";
  }
  setTimeout(() => { zeroNote.textContent = ""; }, 3000);
});

// ---------------- LIVE DATA ----------------
// The board pushes a reading 50 times a second over a WebSocket (/ws). If the socket is not available the page
// asks for /data instead. Either way only the newest reading is kept; drawing happens in animate() at the screen's rate.
let latest = null;          // newest reading from the board
let ws = null;

function connectWS() {
  try {
    ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
    ws.onopen = () => ws.send("stream");        // tells the board to start pushing readings to this page
    ws.onmessage = (e) => {
      try {
        const j = JSON.parse(e.data);
        if (j?.channels) latest = j;
      } catch {}
    };
    ws.onclose = ws.onerror = () => { ws = null; setTimeout(connectWS, 1000); };
  } catch {
    ws = null;
  }
}
connectWS();

let pollBusy = false;
async function pollOnce() {
  if (pollBusy) return;
  pollBusy = true;
  try {
    const r = await fetch("/data", { cache: "no-store" });
    if (r.ok) {
      const json = await r.json();
      if (json?.channels) latest = json;
    }
  } catch {
    // backend not running -> keep what we have
  }
  pollBusy = false;
}
setInterval(() => { if (!(ws && ws.readyState === 1)) pollOnce(); }, 100);   // fallback only

// ---------------- DRAWING ----------------
const smooth = { N: 0, S: 0, E: 0, W: 0 };       // what is drawn: eases towards the newest reading
const SMOOTH_TIME = 0.045;                        // seconds; hides the 20 ms steps without adding noticeable delay
let lastFrame = performance.now();
let lastPanel = 0;
let lastChart = 0;

function updatePanel(d) {
  const ch = d.channels;
  document.getElementById("panel").innerHTML = `
    <b>Ropes</b><br>
    N: ${(ch.ch2.rope ?? 0).toFixed(2)} mm<br>
    S: ${(ch.ch3.rope ?? 0).toFixed(2)} mm<br>
    E: ${(ch.ch4.rope ?? 0).toFixed(2)} mm<br>
    W: ${(ch.ch5.rope ?? 0).toFixed(2)} mm<br><br>

    <b>Forces</b><br>
    Axial: ${d.forces.axial.toFixed(2)} N<br><br>

    <b>Torques</b><br>
    Yaw: ${d.forces.yaw.toFixed(3)} Nm<br>
    Pitch: ${d.forces.pitch.toFixed(3)} Nm
    ${d.valid === false ? "<br><br><span style='color:#f66'>sensors not ready</span>" : d.degraded ? "<br><br><span style='color:#fc6'>3 of 4 sensors</span>" : ""}
  `;
}

function updateCharts(d) {
  labels.push(Date.now());
  forceData.push(d.forces.axial);
  yawData.push(d.forces.yaw);
  pitchData.push(d.forces.pitch);
  while (labels.length > maxPoints) {
    labels.shift();
    forceData.shift();
    yawData.shift();
    pitchData.shift();
  }
  forceChart.update("none");
  torqueChart.update("none");
}

function animate(now) {
  requestAnimationFrame(animate);
  const dt = Math.min(0.1, (now - lastFrame) / 1000);
  lastFrame = now;

  const d = latest ?? FALLBACK_DATA;
  const ch = d.channels;
  const target = { N: ch.ch2.rope ?? 0, S: ch.ch3.rope ?? 0, E: ch.ch4.rope ?? 0, W: ch.ch5.rope ?? 0 };
  const k = 1 - Math.exp(-dt / SMOOTH_TIME);
  for (const key of ["N", "S", "E", "W"]) smooth[key] += (target[key] - smooth[key]) * k;
  updateDisk(smooth.N, smooth.S, smooth.E, smooth.W);

  if (now - lastPanel > 80) { lastPanel = now; updatePanel(d); }          // the numbers: about 12 times a second
  if (now - lastChart > 100) { lastChart = now; if (latest) updateCharts(d); }   // the graphs are the expensive part: 10 Hz

  renderer.render(scene, camera);
}
requestAnimationFrame(animate);

// Resize handling
window.addEventListener("resize", () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});
