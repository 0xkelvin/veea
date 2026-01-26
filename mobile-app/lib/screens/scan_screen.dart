import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:provider/provider.dart';
import '../providers/camera_provider.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  @override
  void initState() {
    super.initState();
    // Start scanning when screen opens - delayed to allow UI to render first
    Future.delayed(const Duration(milliseconds: 100), () {
      if (mounted) {
        context.read<CameraProvider>().startScan();
      }
    });
  }

  @override
  void dispose() {
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<CameraProvider>(
      builder: (context, provider, child) {
        // Navigate back when connected
        if (provider.isConnected) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted) {
              Navigator.pop(context);
            }
          });
        }

        return Scaffold(
          appBar: AppBar(
            title: const Text('Find Veea Device'),
            backgroundColor: Theme.of(context).colorScheme.inversePrimary,
          ),
          body: Column(
            children: [
              // Status banner
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(16),
                color: _getStatusColor(provider.connectionStatus),
                child: Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    if (provider.connectionStatus == ConnectionStatus.scanning ||
                        provider.connectionStatus == ConnectionStatus.connecting)
                      const Padding(
                        padding: EdgeInsets.only(right: 12),
                        child: SizedBox(
                          width: 20,
                          height: 20,
                          child: CircularProgressIndicator(
                            strokeWidth: 2,
                            color: Colors.white,
                          ),
                        ),
                      ),
                    Text(
                      _getStatusText(provider.connectionStatus),
                      style: const TextStyle(
                        color: Colors.white,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ],
                ),
              ),

              // Device list
              Expanded(
                child: provider.scanResults.isEmpty
                    ? _buildSearching()
                    : ListView.builder(
                        itemCount: provider.scanResults.length,
                        itemBuilder: (context, index) {
                          final result = provider.scanResults[index];
                          return _buildDeviceTile(context, result, provider);
                        },
                      ),
              ),
            ],
          ),
          floatingActionButton: FloatingActionButton(
            onPressed: () {
              if (provider.connectionStatus == ConnectionStatus.scanning) {
                provider.stopScan();
              } else {
                provider.startScan();
              }
            },
            child: Icon(
              provider.connectionStatus == ConnectionStatus.scanning
                  ? Icons.stop
                  : Icons.refresh,
            ),
          ),
        );
      },
    );
  }

  Color _getStatusColor(ConnectionStatus status) {
    switch (status) {
      case ConnectionStatus.scanning:
        return Colors.blue;
      case ConnectionStatus.connecting:
        return Colors.orange;
      case ConnectionStatus.connected:
        return Colors.green;
      case ConnectionStatus.disconnected:
        return Colors.grey;
    }
  }

  String _getStatusText(ConnectionStatus status) {
    switch (status) {
      case ConnectionStatus.scanning:
        return 'Scanning for devices...';
      case ConnectionStatus.connecting:
        return 'Connecting...';
      case ConnectionStatus.connected:
        return 'Connected!';
      case ConnectionStatus.disconnected:
        return 'Tap refresh to scan';
    }
  }

  Widget _buildSearching() {
    return const Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(
            Icons.bluetooth_searching,
            size: 80,
            color: Colors.blue,
          ),
          SizedBox(height: 16),
          Text(
            'Looking for Veea devices...',
            style: TextStyle(fontSize: 16),
          ),
          SizedBox(height: 8),
          Text(
            'Make sure your device is powered on\nand advertising via BLE',
            textAlign: TextAlign.center,
            style: TextStyle(color: Colors.grey),
          ),
        ],
      ),
    );
  }

  Widget _buildDeviceTile(
    BuildContext context,
    ScanResult result,
    CameraProvider provider,
  ) {
    final device = result.device;
    final name = device.platformName.isNotEmpty ? device.platformName : 'Unknown';
    final rssi = result.rssi;

    return ListTile(
      leading: Container(
        padding: const EdgeInsets.all(8),
        decoration: BoxDecoration(
          color: Colors.blue.shade100,
          borderRadius: BorderRadius.circular(8),
        ),
        child: const Icon(Icons.devices, color: Colors.blue),
      ),
      title: Text(name),
      subtitle: Text('Signal: $rssi dBm'),
      trailing: ElevatedButton(
        onPressed: provider.connectionStatus == ConnectionStatus.connecting
            ? null
            : () => provider.connect(device),
        child: const Text('Connect'),
      ),
      onTap: provider.connectionStatus == ConnectionStatus.connecting
          ? null
          : () => provider.connect(device),
    );
  }
}
