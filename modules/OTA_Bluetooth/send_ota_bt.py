#!/usr/bin/env python3

"""
ESP32 Bluetooth OTA Update Tool v4.0 - COMPATIBLE CON BUFFER RX

Cambios en v4.0:
- Compatibilidad con RX buffer del ESP32
- Sincronización mejorada
- Validación de ACK antes de siguiente paquete

Protocolo:
- [0x01] + [size_4_bytes] = START_OTA (5 bytes)
- [0x02] + [len_2_bytes] + [data] = DATA_CHUNK (3 + len bytes)
- [0x03] = END_OTA (1 byte)
- 0xAA = ACK
- 0xFF = NAK

Uso:
python3 send_ota_v4.py COM9 build/app.bin
"""

import serial
import sys
import time
import os
import struct

# Protocolo
PROTO_START_OTA = 0x01
PROTO_DATA_CHUNK = 0x02
PROTO_END_OTA = 0x03
PROTO_ACK = 0xAA
PROTO_NAK = 0xFF

def send_firmware_ota(port, firmware_path, baud_rate=115200, chunk_size=1021):
    """
    Envía firmware OTA por SPP v4.0 con sincronización mejorada
    """
    
    # Validar archivo
    if not os.path.isfile(firmware_path):
        print(f"❌ Archivo no existe: {firmware_path}")
        return False
    
    firmware_size = os.path.getsize(firmware_path)
    print(f"📦 Archivo: {firmware_path}")
    print(f"📏 Tamaño: {firmware_size} bytes ({firmware_size / 1024:.2f} KB)")
    print(f"   Tamaño en hex: 0x{firmware_size:08X}")
    
    if chunk_size > 1021:
        chunk_size = 1021
        print(f"⚠️  Chunk size ajustado a {chunk_size}")
    
    try:
        print(f"🔌 Conectando a {port}...")
        ser = serial.Serial(
            port=port,
            baudrate=baud_rate,
            timeout=3.0,
            write_timeout=3.0
        )
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        print(f"✅ Conectado\n")
        
        # ========== FASE 1: START_OTA ==========
        print("📤 FASE 1: Iniciando OTA...")
        start_cmd = struct.pack('>BI', PROTO_START_OTA, firmware_size)
        print(f"   Enviando: {start_cmd.hex()}")
        
        ser.write(start_cmd)
        time.sleep(0.3)
        
        response = ser.read(1)
        if len(response) == 0 or response[0] != PROTO_ACK:
            print("❌ START_OTA no aceptado")
            ser.close()
            return False
        
        print("✅ ESP32 listo\n")
        
        # ========== FASE 2: Envío de datos ==========
        print(f"📤 FASE 2: Enviando {firmware_size} bytes en chunks de {chunk_size}...")
        
        with open(firmware_path, 'rb') as f:
            chunk_num = 0
            bytes_sent = 0
            start_time = time.time()
            
            while bytes_sent < firmware_size:
                chunk_data = f.read(chunk_size)
                chunk_len = len(chunk_data)
                
                if chunk_len == 0:
                    break
                
                chunk_num += 1
                
                # Construir paquete: [0x02][LEN_H][LEN_L][data...]
                packet = struct.pack('>BH', PROTO_DATA_CHUNK, chunk_len) + chunk_data
                
                # Enviar
                try:
                    ser.write(packet)
                    time.sleep(0.02)  # Dar tiempo al ESP32
                    
                    # Esperar ACK
                    response = ser.read(1)
                    
                    if len(response) == 0:
                        print(f"   ⚠️  Timeout chunk {chunk_num}")
                        # Retroceder para reintentar
                        f.seek(bytes_sent)
                        continue
                    
                    if response[0] == PROTO_ACK:
                        bytes_sent += chunk_len
                    else:
                        print(f"   ❌ NAK en chunk {chunk_num}")
                        # Retroceder
                        f.seek(bytes_sent)
                        continue
                        
                except Exception as e:
                    print(f"   ❌ Error: {e}")
                    f.seek(bytes_sent)
                    continue
                
                # Progreso
                if chunk_num % 50 == 0:
                    elapsed = time.time() - start_time
                    if elapsed > 0:
                        speed = bytes_sent / elapsed / (1024 * 1024)
                        pct = (bytes_sent / firmware_size) * 100
                        print(f"   [{chunk_num:4d}] {bytes_sent / 1024:8.1f} KB ({pct:5.1f}%) - {speed:.2f} MB/s")
        
        elapsed = time.time() - start_time
        if elapsed > 0:
            speed = firmware_size / elapsed / (1024 * 1024)
        else:
            speed = 0
        print(f"   ✅ Transferencia: {chunk_num} chunks en {elapsed:.2f}s ({speed:.2f} MB/s)\n")
        
        # ========== FASE 3: END_OTA ==========
        print("📤 FASE 3: Finalizando OTA...")
        end_cmd = bytes([PROTO_END_OTA])
        
        ser.write(end_cmd)
        time.sleep(0.5)
        
        response = ser.read(1)
        if len(response) == 0 or response[0] != PROTO_ACK:
            print("❌ END_OTA no confirmado")
            ser.close()
            return False
        
        print("✅ OTA completada - ESP32 reiniciando...\n")
        
        ser.close()
        return True
        
    except serial.SerialException as e:
        print(f"❌ Error serial: {e}")
        return False
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
        return False

def main():
    print("=" * 70)
    print("  ESP32 Bluetooth OTA v4.0 (Buffer RX sincronizado)")
    print("=" * 70)
    
    if len(sys.argv) < 3:
        print("\n❌ Uso: python3 send_ota_v4.py <puerto> <firmware.bin>")
        print("\nEjemplos:")
        print("  python3 send_ota_v4.py COM9 build/app.bin")
        sys.exit(1)
    
    port = sys.argv[1]
    firmware_path = sys.argv[2]
    
    success = send_firmware_ota(port, firmware_path)
    
    print("=" * 70)
    if success:
        print("✅ COMPLETADO")
    else:
        print("❌ FALLIDO")
    print("=" * 70)
    
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
