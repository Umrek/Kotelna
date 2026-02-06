import requests
import os

# Konfigurace
ESP_IP = "http://192.168.2.9"  # Doplň svou IP
TARGET_FOLDER = "data"
TARGET_FILE = "history.csv"

def download_and_save_history():
    # 1. Vytvoření složky 'data', pokud neexistuje
    if not os.path.exists(TARGET_FOLDER):
        os.makedirs(TARGET_FOLDER)
        print(f"Vytvořena složka: {TARGET_FOLDER}")

    url = f"{ESP_IP}/api/history"
    full_path = os.path.join(TARGET_FOLDER, TARGET_FILE)

    try:
        print(f"Stahuji data z {url}...")
        response = requests.get(url, timeout=20)
        
        if response.status_code == 200:
            raw_data = response.text
            
            if raw_data == "no_data":
                print("ESP32 nemá žádná data k odeslání.")
                return

            # 2. Uložení dat do souboru
            # Používáme kódování utf-8 a mód 'w' (přepsat) nebo 'a' (přidat)
            with open(full_path, "w", encoding="utf-8", newline='') as f:
                f.write(raw_data)
            
            print(f"Úspěch! Data uložena do: {full_path}")
            print(f"Velikost souboru: {os.path.getsize(full_path)} bajtů")
            
        else:
            print(f"Chyba serveru: {response.status_code}")

    except Exception as e:
        print(f"Nastala chyba: {e}")

if __name__ == "__main__":
    download_and_save_history()