# CoolGrid PWA — Remote Access via Firebase

Mobile/desktop installable app for the CoolGrid prototype. Works from anywhere
with internet, since data flows through Firebase Realtime Database.

```
ESP32  ──HTTPS──▶  Firebase RTDB  ──HTTPS──▶  PWA on phone/laptop
       writes              reads
       reads               writes
       commands            commands
```

---

## Files

| File                      | Purpose                                         |
|---------------------------|-------------------------------------------------|
| `index.html`              | The PWA — main dashboard                        |
| `manifest.webmanifest`    | PWA manifest — makes it installable             |
| `sw.js`                   | Service worker — offline support                |
| `icon-192.png`, `icon-512.png`, `icon-maskable-512.png` | App icons |
| `firmware_firebase.ino`   | ESP32 firmware — talks to Firebase              |

---

## Setup walkthrough

### Step 1 — Create the Firebase project (15 min)

1. Go to https://console.firebase.google.com
2. Click **Add project** → name it (e.g. `coolgrid-prototype`) → continue → disable Analytics (not needed) → create.
3. In the project dashboard, click the **Web** icon (`</>`) to register a web app:
   - App nickname: `CoolGrid PWA`
   - Skip Firebase Hosting for now
   - Click **Register app**
4. Copy the `firebaseConfig` object that appears. You'll paste it into `index.html` later.
5. In the left sidebar → **Build** → **Realtime Database** → **Create Database**:
   - Region: closest to Malaysia → **Singapore (asia-southeast1)**
   - Start in **locked mode** (we'll fix rules next)
6. Go to the **Rules** tab and paste:
   ```json
   {
     "rules": {
       "coolgrid": {
         ".read":  "auth != null",
         ".write": "auth != null"
       }
     }
   }
   ```
   Click **Publish**.
7. Sidebar → **Build** → **Authentication** → **Get Started** → enable **Email/Password** provider.
8. **Users** tab → **Add user**:
   - Email: `device@coolgrid.local`
   - Password: pick a strong one (you'll need it twice)
   - Click **Add user**. Note the email + password — both ESP32 and PWA will use them.

### Step 2 — Configure the PWA (5 min)

Open `index.html` and find this block near the bottom:
```js
const firebaseConfig = {
  apiKey:        "YOUR_API_KEY",
  authDomain:    "YOUR_PROJECT.firebaseapp.com",
  ...
};
```
Replace with the config you copied in step 1.4.

> Since the database rules require auth, the PWA also needs to sign in.
> If you want to lock it down further (recommended), see "Adding PWA auth" below.

### Step 3 — Host the PWA on GitHub Pages (10 min)

A PWA needs HTTPS to be installable. GitHub Pages gives you that free.

```bash
# Create a new GitHub repo named "coolgrid-pwa" then:
cd coolgrid-pwa
git init
git add .
git commit -m "initial PWA"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/coolgrid-pwa.git
git push -u origin main
```

On GitHub:
1. Repo settings → **Pages**
2. Source: **Deploy from a branch** → **main** → `/` (root)
3. Save. After ~1 min you'll get a URL like `https://YOUR_USERNAME.github.io/coolgrid-pwa/`

### Step 4 — Configure the ESP32 firmware (5 min)

Open `firmware_firebase.ino` and edit the CONFIG block:
```cpp
#define WIFI_SSID         "your_wifi"
#define WIFI_PASSWORD     "your_pass"
#define API_KEY           "<from firebaseConfig.apiKey>"
#define DATABASE_URL      "<from firebaseConfig.databaseURL>"
#define USER_EMAIL        "device@coolgrid.local"
#define USER_PASSWORD     "<the password you set in step 1.8>"
```

### Step 5 — Install the Firebase ESP library (5 min)

In Arduino IDE:
1. **Sketch** → **Include Library** → **Manage Libraries**
2. Search: `Firebase ESP Client`
3. Install **Firebase ESP Client** by **Mobizt** (v4.x recommended)

Flash the firmware. Open Serial Monitor at 115200 baud — you should see:
```
WiFi connected. IP: 192.168.x.x
Firebase authenticated.
Listening for commands at /coolgrid/cmd
[Firebase] telemetry pushed
```

### Step 6 — Install the PWA on your phone (1 min)

**Android (Chrome):**
1. Open the GitHub Pages URL in Chrome
2. Tap the **⋮** menu → **Install app** (or **Add to Home screen**)
3. The CoolGrid icon appears on your home screen. Tap to open — it runs full-screen like a real app.

**iOS (Safari):**
1. Open the GitHub Pages URL in Safari
2. Tap the **Share** icon → **Add to Home Screen**
3. The CoolGrid icon appears on your home screen.

**Desktop (Chrome/Edge):**
1. Open the URL → look for an install icon in the address bar
2. Click it — opens as a standalone desktop window

---

## Adding PWA auth (recommended before competition demo)

Right now the PWA doesn't sign in to Firebase, so the writes will be rejected
by the rules. Add this to the top of the `<script type="module">` block in
`index.html`:

```js
import { getAuth, signInWithEmailAndPassword } from "https://www.gstatic.com/firebasejs/10.12.0/firebase-auth.js";
const auth = getAuth(app);
await signInWithEmailAndPassword(auth, "device@coolgrid.local", "YOUR_PASSWORD");
```

For a real product you'd give each user their own account, but for a
competition demo using the same device account works fine.

---

## Troubleshooting

| Symptom                         | Likely cause                                                  |
|--------------------------------|---------------------------------------------------------------|
| PWA shows "Reconnecting..."     | Firebase config wrong, or PWA didn't authenticate            |
| ESP32 logs `Firebase auth timed out` | Wrong email/password, or DATABASE_URL wrong              |
| ESP32 logs `push failed: permission_denied` | Database rules not published or auth user missing |
| Commands sent from PWA but pump doesn't respond | Stream not begun — check serial output for "Listening" |
| `Stream begin failed`           | DATABASE_URL is missing the `https://` prefix or trailing `firebaseio.com` |
| Install button doesn't appear   | PWA not served over HTTPS (GitHub Pages must be enabled)     |

---

## Data structure in Firebase

```
coolgrid/
├── telemetry/                     ← ESP32 writes every 5s
│   ├── device_id: "coolgrid-esp32-01"
│   ├── dht11_temp: 31.2
│   ├── dht11_hum:  68.5
│   ├── dht22_temp: 33.1
│   ├── dht22_hum:  65.2
│   ├── water_temp: 36.4
│   ├── cistern_pct: 72
│   ├── cistern_cm:  12.5
│   ├── pump_on:    true
│   ├── auto_mode:  true
│   └── pump_runtime_hours: 2.34
└── cmd/                           ← PWA writes when buttons tapped
    ├── cmd: "AUTO"
    ├── ts:  1759123456789
    └── src: "pwa"
```

---

## Free tier limits

Firebase free tier (Spark plan):
- Realtime Database: 1 GB stored, 10 GB/month downloaded, 100 concurrent connections.
- For this prototype (one ESP32 + a few PWA clients) this is far more than enough.
