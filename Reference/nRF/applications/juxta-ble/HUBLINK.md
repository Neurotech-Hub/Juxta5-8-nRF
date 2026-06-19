## BLE Characteristics & Protocol

To support "Hublink" BLE gateways that will scan and connect to our periperal, we require a custom BLE service with four characteristics for file transfer and device management. All characteristics use the service UUID: `57617368-5501-0001-8000-00805f9b34fb`. This peripheral shoud always have an advertising name of "JX_*" where "*" is the last size characters of this devices MAC address; this is considered the device_id below.

Required characteristics:

### 1. Node Characteristic (READ)
**UUID**: `57617368-5505-0001-8000-00805f9b34fb`

Provides device status and configuration information in JSON format:
```json
{
  "upload_path": "/TEST",
  "firmware_version": "1.0.0",
  "battery_level": 85,
  "device_id": "JX_*",
  "alert": ""
}
```

**Fields**:
- `upload_path` (string): Base path for file uploads
- `firmware_version` (string): Current peripheral software version
- `battery_level` (number): Battery level 0-255 (0 = not set, only present if > 0)
- `device_id` (string): Hardware device identifier
- `alert` (string): Alert message (only present if set by user, auto-clears after read)

**Usage**: Gateway reads this characteristic after connection to get device information and status.

### 2. Gateway Characteristic (WRITE)
**UUID**: `57617368-5504-0001-8000-00805f9b34fb`

Accepts JSON commands to control device behavior. Multiple commands can be sent in a single JSON object:

```json
{
  "timestamp": 1234567890,
  "sendFilenames": true,
  "clearMemory": true,
  "advInterval": 5,
  "scanInterval": 15,
  "subjectId":"vole001",
  "uploadPath":"/TEST",
  "operatingMode": 0
}
```

This implementaion is unique from other nodes that have an internal memory card where subjectId and uploadPath would be manually set/written. Here, we must rely on the BLE connection itself.

**Commands**:
- `timestamp` (number): Unix timestamp for device synchronization
- `sendFilenames` (boolean): Triggers file listing process when true
- `clearMemory` (boolean): Clears device memory when true
- `advInterval` (integer): set advertising burst interval, 0 means no advertising
- `scanInterval` (integer): set scanning burst interval, 0 means no scanning
- `subjectId` (string): should be saved internally
- `uploadPath` (string): should be saved internally
- `operatingMode` (integer): set device operating mode (0 = NORMAL mode with BLE bursts/motion counting, 1 = ADC_ONLY mode with pure ADC recordings)

**Usage**: Write JSON commands to control device behavior. Device responds via callbacks.

### 3. Filename Characteristic (READ/WRITE/INDICATE)
**UUID**: `57617368-5502-0001-8000-00805f9b34fb`

**WRITE**: This characteristic will receive a filename as a request for file transfer:
```
"data.txt"
```

**INDICATE**: Receives file listing or transfer status
- **File listing format**: `"filename1.txt|1234;filename2.csv|5678;EOF"`
  - Each file: `"filename|filesize"`
  - Separator: `;`
  - End marker: `"EOF"`
- **Transfer status**: `"NFF"` (No File Found) if requested file doesn't exist

**Usage**: 
1. Write filename to request transfer
2. Subscribe to indications for file listing or status updates

### 4. File Transfer Characteristic (READ/INDICATE)
**UUID**: `57617368-5503-0001-8000-00805f9b34fb`

**INDICATE**: Sends file content in chunks
- **Data chunks**: Raw file bytes (MTU-sized, typically 512 bytes)
- **End marker**: `"EOF"` when transfer complete
- **Error marker**: `"NFF"` if file not found

**Usage**: Subscribe to indications to receive file content. Monitor for "EOF" or "NFF" markers.

## Connection Protocol

### 1. Device Discovery
- **Service UUID**: `57617368-5501-0001-8000-00805f9b34fb`
- **Advertising Name**: "JX_*"
- **MTU Size**: Device negotiates to 515 bytes (512 + 3 byte header)

### 2. Connection Sequence
1. **Connect** to device
2. **Read Node Characteristic** to get device info
3. **Gateway Subscribes to indications** on Filename and File Transfer characteristics
4. **Device Writes to Gateway Characteristic** and may initiate file transfer

### 3. File Transfer Workflow

#### File Listing
1. Gateway writes `{"sendFilenames": true}` to Gateway Characteristic
2. Gateway receives file list via Filename Characteristic indications in format: `"filename|size;filename2|size2;EOF"`
3. Gateway will decide what files are required for File Download.

#### File Download
1. Filename is written to Filename Characteristic
2. Gateway receives file content via File Transfer Characteristic indications
3. Gateway monitors for "EOF" or "NFF" markers

### 4. Other Details

- Timeout: None for now, but this device must handle any disconnection gracefully (automatic cleanup) via callbacks and state management

- Error Handling
- **File not found**: Device sends "NFF" via Filename Characteristic
- **Transfer errors**: Device disconnects and resets state

- State Management
- **Alert messages**: Auto-clear after each sync cycle
- **Battery level**: Persists until next update
- **BLE state**: Reset between cycles; Node Characteristic is ideally updated with proper information