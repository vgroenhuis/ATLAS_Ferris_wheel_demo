const gameArea = document.querySelector(".thegame");
const sliderX = document.getElementById("sliderX");
const sliderY = document.getElementById("sliderY");
const scoreDisplay = document.getElementById("scoreDisplay");
const timeDisplay = document.getElementById("timeDisplay");

let player, target, startButton;
let score = 0;
let timeLeft = 30;
let gameRunning = false;

let posX, posY;
let velocityX = 0;
let velocityY = 0;

let speedMultiplier = 100000;

let radius;
let center;

let sensorVX = 0;
let sensorVY = 0;

let fetchBusy = false;

const demoUsernames = [
  "PixelRanger",
  "NovaTrail",
  "EchoBlitz",
  "ShadowPickle",
  "TurboLynx",
  "CrimsonByte",
  "FrostNova",
  "QuantumDuck",
  "NeonBadger",
  "SolarPing",
  "MintyMeteor",
  "RocketMango",
  "BlueComet",
  "VelvetFalcon",
  "CyberPanda",
  "TurboWaffle",
  "LunarPickaxe",
  "CosmoSprinter",
  "ElectricOtter",
  "ByteBandit",
  "SapphireNinja",
  "CloudRaccoon",
  "HyperKiwi",
  "IronCupcake",
  "GammaSprout",
  "StellarToad",
  "NovaCarrot",
  "PixelSamurai",
  "OrbitPenguin",
  "TurboCactus"
];

let highscores = [];
let playerCounter = 1;

function updateGameDimensions() {
    const rect = gameArea.getBoundingClientRect();
    radius = rect.width / 2;
    center = radius;

    // Recenter start button if it exists
    if (startButton) {
        startButton.style.left = center + "px";
        startButton.style.top = center + "px";
    }
}

updateGameDimensions();
window.addEventListener("resize", updateGameDimensions);


// ---------------- SENSOR FETCH ----------------
async function fetchSensorData() {
    if (fetchBusy) return;
    fetchBusy = true;

    // ---- Fetch Sensor Data from Backend ----
    try {
        const r = await fetch("/data");
        if (r.ok) {
            const json = await r.json();
            if (json?.channels) {

                const ch = json.channels;

                const N = ch.ch2.rope ?? 0;
                const S = ch.ch3.rope ?? 0;
                const E = ch.ch4.rope ?? 0;
                const W = ch.ch5.rope ?? 0;

                // ---- DIFFERENTIAL MAPPING ----
                const rawX = (E - W);
                const rawY = (S - N);

                // ---- Normalize (tune this factor!) ----
                const scale = 0.002;   // <-- you will tune this
                sensorVX = rawX * scale;
                sensorVY = rawY * scale;
            }
        }
    } catch {
        // ignore if backend not running
    }

    fetchBusy = false;
}

// Poll at 20 Hz
setInterval(fetchSensorData, 50);

// Create Player
function createPlayer() {
    player = document.createElement("div");
    player.classList.add("player");
    gameArea.appendChild(player);

    posX = center;
    posY = center;
    updatePlayerPosition();
}

// Create Target
function spawnTarget() {
    if (target) target.remove();

    target = document.createElement("div");
    target.classList.add("target");

    let angle = Math.random() * Math.PI * 2;
    let r = Math.random() * (radius - 15);

    let x = center + r * Math.cos(angle);
    let y = center + r * Math.sin(angle);

    target.style.left = x + "px";
    target.style.top = y + "px";

    gameArea.appendChild(target);
}

// Update Player Position
function updatePlayerPosition() {
    player.style.left = posX + "px";
    player.style.top = posY + "px";
}

// Collision Check
function checkCollision() {
    const dx = posX - parseFloat(target.style.left);
    const dy = posY - parseFloat(target.style.top);
    const distance = Math.sqrt(dx * dx + dy * dy);

    if (distance < 15) {
        score += 10;
        scoreDisplay.textContent = "Score: " + score;
        spawnTarget();
    }
}


// Add a new score when a game finishes
function addScore(score, baseName) {

    const number = String(playerCounter).padStart(2, "0");
    const username = `${baseName}_${number}`;

    playerCounter++;

    highscores.push({
        name: username,
        score: score
    });

    // Sort highest first
    highscores.sort((a, b) => b.score - a.score);

    renderHighscores();
}

function renderHighscores() {

    const container = document.querySelector(".highscores");

    let html = "<h3>Highscores</h3>";

    highscores.forEach((player, index) => {

        html += `
        <div class="score-row">
            <span class="rank">#${index + 1}</span>
            <span class="name">${player.name}</span>
            <span class="score">${player.score}</span>
        </div>`;
    });

    container.innerHTML = html;
}

// Game Loop
function gameLoop() {
    if (!gameRunning) return;

    velocityX = sensorVX * speedMultiplier;
    velocityY = sensorVY * speedMultiplier;

    posX += velocityX * 0.016;
    posY += velocityY * 0.016;

    // Constrain inside circle
    const dx = posX - center;
    const dy = posY - center;
    const distance = Math.sqrt(dx * dx + dy * dy);

    if (distance > radius - 8) {
        const angle = Math.atan2(dy, dx);
        posX = center + (radius - 8) * Math.cos(angle);
        posY = center + (radius - 8) * Math.sin(angle);
    }

    updatePlayerPosition();
    checkCollision();

    requestAnimationFrame(gameLoop);
}

// Timer
function startTimer() {
    const timerInterval = setInterval(() => {
        timeLeft--;
        timeDisplay.textContent = "Time: " + timeLeft;

        if (timeLeft <= 0) {
            clearInterval(timerInterval);
            gameRunning = false;
            const creative_name = demoUsernames[Math.floor(Math.random() * demoUsernames.length)];

            addScore(score, creative_name);

            // Show final score inside play area
            startButton.style.display = "block";
            target.style.display = "none";
            player.style.display = "none";

            startButton.innerHTML = `
                <div style="text-align:center;">
                    <div style="font-size:1.5em; margin-bottom:10px;">
                        <b>${creative_name}</b> Score: ${score}
                    </div>
                    <hr style="border:1px solid #5B5F66;" />
                    <div style="font-size:1.5em;" ><b>Play Again</b></div>
                </div>
            `;
        }
    }, 1000);
}

// Start Game
function startGame() {
    score = 0;
    timeLeft = 30;
    scoreDisplay.textContent = "Score: 0";
    timeDisplay.textContent = "Time: 30";

    posX = center;
    posY = center;

    spawnTarget();

    gameRunning = true;
    startButton.style.display = "none";
    player.style.display = "block";

    startTimer();
    requestAnimationFrame(gameLoop);
}

// Create Start Button
function createStartButton() {
    startButton = document.createElement("button");
    startButton.classList.add("startButton");
    startButton.textContent = "Start";
    startButton.style.fontWeight = "bold";

    startButton.style.left = center + "px";
    startButton.style.top = center + "px";

    startButton.addEventListener("click", startGame);
    gameArea.appendChild(startButton);
}

// Initialize
createPlayer();
createStartButton();
renderHighscores()
