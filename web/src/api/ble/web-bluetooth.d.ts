// Minimal Web Bluetooth typings (only what bleTransport.ts uses).
interface BluetoothRemoteGATTCharacteristic extends EventTarget {
  readonly value?: DataView;
  readValue(): Promise<DataView>;
  writeValueWithoutResponse?(value: BufferSource): Promise<void>;
  writeValueWithResponse?(value: BufferSource): Promise<void>;
  writeValue?(value: BufferSource): Promise<void>;
  startNotifications(): Promise<BluetoothRemoteGATTCharacteristic>;
}
interface BluetoothRemoteGATTService {
  getCharacteristic(uuid: string): Promise<BluetoothRemoteGATTCharacteristic>;
}
interface BluetoothRemoteGATTServer {
  readonly connected: boolean;
  connect(): Promise<BluetoothRemoteGATTServer>;
  disconnect(): void;
  getPrimaryService(uuid: string): Promise<BluetoothRemoteGATTService>;
}
interface BluetoothDevice extends EventTarget {
  readonly id: string;
  readonly name?: string;
  readonly gatt?: BluetoothRemoteGATTServer;
}
interface BluetoothRequestDeviceOptions {
  filters?: { services?: string[]; namePrefix?: string }[];
  optionalServices?: string[];
}
interface Bluetooth {
  requestDevice(options: BluetoothRequestDeviceOptions): Promise<BluetoothDevice>;
  getDevices?(): Promise<BluetoothDevice[]>;
}
interface Navigator {
  readonly bluetooth?: Bluetooth;
}
