package com.example.nfcapp;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.os.Build;
import android.os.Handler;
import android.provider.Settings;
import android.util.Log;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public class NFCBluetoothManager {
    private static final String TAG = "NFCBluetoothManager";
    private static final String TARGET_DEVICE_NAME = "NFC-IOP";
    private static final UUID SERVICE_UUID = UUID.fromString("0000FFF0-0000-1000-8000-00805F9B34FB");
    private static final UUID READ_UUID = UUID.fromString("0000FFF1-0000-1000-8000-00805F9B34FB");
    private static final UUID WRITE_UUID = UUID.fromString("0000FFF2-0000-1000-8000-00805F9B34FB");

    private Context context;
    private Activity activity;
    private BluetoothAdapter bluetoothAdapter;
    private BluetoothLeScanner bluetoothLeScanner;
    private BluetoothGatt bluetoothGatt;
    private BluetoothGattCharacteristic readCharacteristic;
    private BluetoothGattCharacteristic writeCharacteristic;

    private Handler scanHandler = new Handler();
    private boolean isScanning = false;
    private boolean isConnected = false;

    private OnDeviceFoundListener deviceFoundListener;
    private OnConnectionStateChangeListener connectionStateListener;
    private OnDataReceivedListener dataReceivedListener;

    public interface OnDeviceFoundListener {
        void onDeviceFound(BluetoothDevice device);
        void onNoDeviceFound();
    }

    public interface OnConnectionStateChangeListener {
        void onConnectionStateChanged(boolean connected);
        void onConnectionFailed();
    }

    public interface OnDataReceivedListener {
        void onDataReceived(byte[] data);
    }

    public NFCBluetoothManager(Context context, Activity activity) {
        this.context = context;
        this.activity = activity;

        android.bluetooth.BluetoothManager bluetoothManager = (android.bluetooth.BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        bluetoothAdapter = bluetoothManager.getAdapter();

        if (bluetoothAdapter != null) {
            bluetoothLeScanner = bluetoothAdapter.getBluetoothLeScanner();
        }
    }

    public boolean isBluetoothSupported() {
        return bluetoothAdapter != null;
    }

    public boolean isBluetoothEnabled() {
        return bluetoothAdapter != null && bluetoothAdapter.isEnabled();
    }

    public boolean hasLocationPermission() {
        return ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    public boolean hasBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
                   ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
        } else {
            return ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH) == PackageManager.PERMISSION_GRANTED &&
                   ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_ADMIN) == PackageManager.PERMISSION_GRANTED;
        }
    }

    public void requestLocationPermission() {
        ActivityCompat.requestPermissions(activity,
            new String[]{Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION},
            1001);
    }

    public void requestBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ActivityCompat.requestPermissions(activity,
                new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT},
                1002);
        } else {
            ActivityCompat.requestPermissions(activity,
                new String[]{Manifest.permission.BLUETOOTH, Manifest.permission.BLUETOOTH_ADMIN},
                1002);
        }
    }

    public void enableBluetooth() {
        if (bluetoothAdapter != null && !bluetoothAdapter.isEnabled()) {
            Intent enableBtIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            activity.startActivityForResult(enableBtIntent, 1003);
        }
    }

    public void enableLocationServices() {
        Intent intent = new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS);
        activity.startActivity(intent);
    }

    public void startScanning() {
        if (!hasLocationPermission() || !hasBluetoothPermissions() || !isBluetoothEnabled()) {
            Log.e(TAG, "Missing permissions or Bluetooth not enabled");
            return;
        }

        if (isScanning) {
            stopScanning();
        }

        isScanning = true;
        bluetoothLeScanner.startScan(scanCallback);
        scanHandler.postDelayed(this::stopScanning, 10000); // 10秒后停止扫描
    }

    public void stopScanning() {
        if (isScanning && bluetoothLeScanner != null) {
            bluetoothLeScanner.stopScan(scanCallback);
            isScanning = false;
            scanHandler.removeCallbacksAndMessages(null);
        }
    }

    private ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            if (device != null && TARGET_DEVICE_NAME.equals(device.getName())) {
                stopScanning();
                if (deviceFoundListener != null) {
                    deviceFoundListener.onDeviceFound(device);
                }
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            Log.e(TAG, "Scan failed with error code: " + errorCode);
            stopScanning();
            if (deviceFoundListener != null) {
                deviceFoundListener.onNoDeviceFound();
            }
        }
    };

    public void connectToDevice(BluetoothDevice device) {
        if (device == null) return;

        bluetoothGatt = device.connectGatt(context, false, gattCallback);
    }

    public void disconnect() {
        if (bluetoothGatt != null) {
            bluetoothGatt.disconnect();
            bluetoothGatt.close();
            bluetoothGatt = null;
        }
        isConnected = false;
        if (connectionStateListener != null) {
            connectionStateListener.onConnectionStateChanged(false);
        }
    }

    public boolean isConnected() {
        return isConnected;
    }

    public void sendData(String data) {
        Log.d(TAG, "sendData called with: " + data);
        if (writeCharacteristic != null && isConnected && bluetoothGatt != null) {
            try {
                writeCharacteristic.setValue(data.getBytes());
                boolean result = bluetoothGatt.writeCharacteristic(writeCharacteristic);
                Log.d(TAG, "Write characteristic result: " + result);
            } catch (Exception e) {
                Log.e(TAG, "Error sending data", e);
            }
        } else {
            Log.e(TAG, "Cannot send data - writeCharacteristic: " + (writeCharacteristic != null) + 
                  ", isConnected: " + isConnected + ", bluetoothGatt: " + (bluetoothGatt != null));
        }
    }
    
    // 发送字节数组数据（用于16进制数据）
    public void sendData(byte[] data) {
        Log.d(TAG, "sendData called with bytes: " + java.util.Arrays.toString(data));
        if (writeCharacteristic != null && isConnected && bluetoothGatt != null) {
            try {
                // 检查特征值属性
                int properties = writeCharacteristic.getProperties();
                Log.d(TAG, "Write characteristic properties: " + properties);
                
                // 设置写入类型
                if ((properties & BluetoothGattCharacteristic.PROPERTY_WRITE) != 0) {
                    writeCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                } else if ((properties & BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0) {
                    writeCharacteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
                }
                
                writeCharacteristic.setValue(data);
                boolean result = bluetoothGatt.writeCharacteristic(writeCharacteristic);
                Log.d(TAG, "Write characteristic result: " + result);
                
                if (!result) {
                    Log.e(TAG, "Failed to write characteristic");
                }
            } catch (Exception e) {
                Log.e(TAG, "Error sending byte data", e);
            }
        } else {
            Log.e(TAG, "Cannot send data - writeCharacteristic: " + (writeCharacteristic != null) + 
                  ", isConnected: " + isConnected + ", bluetoothGatt: " + (bluetoothGatt != null));
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.d(TAG, "Connected to GATT server.");
                gatt.discoverServices();
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.d(TAG, "Disconnected from GATT server.");
                isConnected = false;
                if (connectionStateListener != null) {
                    connectionStateListener.onConnectionStateChanged(false);
                }
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            Log.d(TAG, "onServicesDiscovered: status=" + status);
            if (status == BluetoothGatt.GATT_SUCCESS) {
                BluetoothGattService service = gatt.getService(SERVICE_UUID);
                if (service != null) {
                    Log.d(TAG, "Service found: " + SERVICE_UUID);
                    readCharacteristic = service.getCharacteristic(READ_UUID);
                    writeCharacteristic = service.getCharacteristic(WRITE_UUID);

                    Log.d(TAG, "Read characteristic: " + (readCharacteristic != null ? READ_UUID : "null"));
                    Log.d(TAG, "Write characteristic: " + (writeCharacteristic != null ? WRITE_UUID : "null"));

                    // 如果找不到指定的特征值，尝试查找所有特征值
                    if (readCharacteristic == null || writeCharacteristic == null) {
                        Log.w(TAG, "Target characteristics not found, searching all characteristics...");
                        for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                            Log.d(TAG, "Found characteristic: " + characteristic.getUuid() + 
                                  " (Properties: " + characteristic.getProperties() + ")");
                            
                            // 查找具有通知属性的特征值作为读取特征值
                            if (readCharacteristic == null && 
                                (characteristic.getProperties() & BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0) {
                                readCharacteristic = characteristic;
                                Log.d(TAG, "Using as read characteristic: " + characteristic.getUuid());
                            }
                            
                            // 查找具有写入属性的特征值作为写入特征值
                            if (writeCharacteristic == null && 
                                ((characteristic.getProperties() & BluetoothGattCharacteristic.PROPERTY_WRITE) != 0 ||
                                 (characteristic.getProperties() & BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0)) {
                                writeCharacteristic = characteristic;
                                Log.d(TAG, "Using as write characteristic: " + characteristic.getUuid());
                            }
                        }
                    }

                    if (readCharacteristic != null) {
                        // 启用通知
                        gatt.setCharacteristicNotification(readCharacteristic, true);
                        
                        // 设置描述符以启用通知
                        if (readCharacteristic.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")) != null) {
                            readCharacteristic.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805F9B34FB"))
                                .setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                            gatt.writeDescriptor(readCharacteristic.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")));
                        }
                        
                        isConnected = true;
                        if (connectionStateListener != null) {
                            connectionStateListener.onConnectionStateChanged(true);
                        }
                        Log.d(TAG, "Connection established successfully");
                    } else {
                        Log.e(TAG, "Read characteristic not found");
                        if (connectionStateListener != null) {
                            connectionStateListener.onConnectionFailed();
                        }
                    }
                } else {
                    Log.e(TAG, "Service not found: " + SERVICE_UUID);
                    if (connectionStateListener != null) {
                        connectionStateListener.onConnectionFailed();
                    }
                }
            } else {
                Log.e(TAG, "Service discovery failed with status: " + status);
                if (connectionStateListener != null) {
                    connectionStateListener.onConnectionFailed();
                }
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            Log.d(TAG, "onCharacteristicChanged: " + characteristic.getUuid());
            if (READ_UUID.equals(characteristic.getUuid()) && dataReceivedListener != null) {
                byte[] data = characteristic.getValue();
                if (data != null && data.length > 0) {
                    Log.d(TAG, "Received data: " + new String(data));
                    dataReceivedListener.onDataReceived(data);
                } else {
                    Log.w(TAG, "Received empty data");
                }
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            Log.d(TAG, "Characteristic write: " + (status == BluetoothGatt.GATT_SUCCESS ? "success" : "failed"));
        }
    };

    public void setDeviceFoundListener(OnDeviceFoundListener listener) {
        this.deviceFoundListener = listener;
    }

    public void setConnectionStateListener(OnConnectionStateChangeListener listener) {
        this.connectionStateListener = listener;
    }

    public void setDataReceivedListener(OnDataReceivedListener listener) {
        this.dataReceivedListener = listener;
    }

    // 添加调试方法
    public void printServiceInfo() {
        if (bluetoothGatt != null) {
            Log.d(TAG, "Available services:");
            for (BluetoothGattService service : bluetoothGatt.getServices()) {
                Log.d(TAG, "Service: " + service.getUuid());
                for (BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                    Log.d(TAG, "  Characteristic: " + characteristic.getUuid() + 
                          " (Properties: " + characteristic.getProperties() + ")");
                }
            }
        }
    }
}