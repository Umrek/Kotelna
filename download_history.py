import os
import sys
import subprocess
try:
    import requests
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "requests"])
    import requests
from datetime import datetime
import shutil


# --- LOGIKA STAHOVÁNÍ ---
def download_logic():
    ESP_IP = "http://192.168.2.9"
    DATA_FOLDER = "data"
    BACKUP_FOLDER = "backups"
    FILENAME = "history.csv"

    # Vytvoření složek
    for folder in [DATA_FOLDER, BACKUP_FOLDER]:
        if not os.path.exists(folder):
            os.makedirs(folder)

    current_file_path = os.path.join(DATA_FOLDER, FILENAME)
    
    # 1. Přesun starého souboru do backups
    if os.path.exists(current_file_path):
        timestamp = datetime.now().strftime("%y%m%d%H%M")
        archive_name = f"history_{timestamp}.csv"
        archive_path = os.path.join(BACKUP_FOLDER, archive_name)
        try:
            shutil.move(current_file_path, archive_path)
            print(f"--> [PYTHON] Archivováno do {archive_path}")
        except Exception as e:
            print(f"--> [PYTHON] Chyba archivace: {e}")

    # 2. Stažení nových dat z ESP
    try:
        print(f"--> [PYTHON] Pokus o stažení dat z {ESP_IP}...")
        r = requests.get(f"{ESP_IP}/api/history", timeout=10)
        if r.status_code == 200:
            if r.text == "no_data":
                print("--> [PYTHON] ESP32 hlásí: Žádná data k dispozici.")
            else:
                with open(current_file_path, "w", encoding="utf-8", newline='') as f:
                    f.write(r.text)
                print(f"--> [PYTHON] Nová historie stažena do {current_file_path}")
        else:
            print(f"--> [PYTHON] ESP32 vrátilo chybu HTTP: {r.status_code}")
    except Exception as e:
        print(f"--> [PYTHON] ESP32 nedostupné (asi je offline): {e}")

# --- INTEGRACE DO PLATFORMIO ---
try:
    from SCons.Script import Import
    Import("env")

    # Definujeme funkci s argumenty, které PlatformIO vyžaduje
    def pio_callback(source, target, env):
        download_logic()

    # Navážeme na akci uploadfs
    env.AddPreAction("uploadfs", pio_callback)
    print("[PIO-SCRIPT] Skript úspěšně zaregistrován pro akci uploadfs.")

except Exception:
    # Pokud skript pouštíš ručně (mimo PIO)
    if __name__ == "__main__":
        download_logic()