# ESP Fish Balance Display

Standalone ESP32-C6 + ST7789 display project. The screen shows only the character image and balance; the Orange Pi fetches and caches the DeepSeek balance so the API key stays off the device.

## Prepare

1. Copy `include/secrets_example.h` to `include/secrets_local.h` and set the 2.4 GHz Wi-Fi credentials and Orange Pi address.
2. Run `python tools/convert_whale.py` after installing Pillow. The generated 112x112 RGB565 bitmap occupies about 25 KB of flash.
3. Install the PlatformIO dependencies and build with `pio run`.
4. On the Orange Pi, create a service account and install the files:

   ```sh
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin espfish
   sudo install -d -o root -g root -m 755 /opt/espfish
   sudo install -o root -g root -m 644 gateway/balance_service.py /opt/espfish/balance_service.py
   sudo install -d -o root -g espfish -m 750 /etc/espfish
   sudo bash -c 'umask 077; printf "DEEPSEEK_API_KEY=" > /etc/espfish/balance.env; IFS= read -r -s -p "DeepSeek API key: " key; printf "%s\\n" "$key" >> /etc/espfish/balance.env; unset key; echo'
   sudo chown root:espfish /etc/espfish/balance.env
   sudo chmod 640 /etc/espfish/balance.env
   sudo install -o root -g root -m 644 gateway/espfish-balance.service /etc/systemd/system/espfish-balance.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now espfish-balance.service
   ```

   Enter the key only at the hidden prompt on the Orange Pi. Do not put it in source, firmware, shell history, or a command-line argument.
5. Upload with `pio run -t upload` after confirming the selected serial port.

The service reads `https://api.deepseek.com/user/balance` once per ESP32 poll, returns the available CNY balance when present, and keeps the last successful result if DeepSeek is temporarily unavailable. It listens on port 8081 and should only be exposed to the trusted LAN.

Character art: `DSniang1.png` from [DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget), MIT License; see `assets/LICENSE-MeteorNOX`.
