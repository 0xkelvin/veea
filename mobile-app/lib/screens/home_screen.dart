import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../providers/camera_provider.dart';
import 'gallery_screen.dart';
import 'scan_screen.dart';

class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<CameraProvider>(
      builder: (context, provider, child) {
        return Scaffold(
          appBar: AppBar(
            title: const Text('Veea Camera'),
            backgroundColor: Theme.of(context).colorScheme.inversePrimary,
            actions: [
              // Connection status indicator
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: Row(
                  children: [
                    Icon(
                      provider.isConnected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
                      color: provider.isConnected ? Colors.green : Colors.grey,
                    ),
                    const SizedBox(width: 4),
                    Text(
                      provider.isConnected ? 'Connected' : 'Disconnected',
                      style: TextStyle(
                        color: provider.isConnected ? Colors.green : Colors.grey,
                        fontSize: 12,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
          body: Column(
            children: [
              // Connection Card
              _buildConnectionCard(context, provider),
              
              // Error message
              if (provider.errorMessage != null)
                Container(
                  width: double.infinity,
                  color: Colors.red.shade100,
                  padding: const EdgeInsets.all(8),
                  child: Row(
                    children: [
                      const Icon(Icons.error, color: Colors.red),
                      const SizedBox(width: 8),
                      Expanded(child: Text(provider.errorMessage!)),
                      IconButton(
                        icon: const Icon(Icons.close),
                        onPressed: provider.clearError,
                      ),
                    ],
                  ),
                ),

              // Gallery
              Expanded(
                child: provider.images.isEmpty
                    ? _buildEmptyState()
                    : const GalleryScreen(),
              ),
            ],
          ),
          floatingActionButton: provider.isConnected
              ? FloatingActionButton.extended(
                  onPressed: provider.isCapturing ? null : () => provider.capture(),
                  icon: provider.isCapturing
                      ? const SizedBox(
                          width: 24,
                          height: 24,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.camera_alt),
                  label: Text(provider.isCapturing ? 'Capturing...' : 'Capture'),
                )
              : FloatingActionButton.extended(
                  onPressed: () {
                    Navigator.push(
                      context,
                      MaterialPageRoute(builder: (_) => const ScanScreen()),
                    );
                  },
                  icon: const Icon(Icons.bluetooth_searching),
                  label: const Text('Connect'),
                ),
        );
      },
    );
  }

  Widget _buildConnectionCard(BuildContext context, CameraProvider provider) {
    return Card(
      margin: const EdgeInsets.all(16),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: provider.isConnected 
                    ? Colors.green.shade100 
                    : Colors.grey.shade200,
                borderRadius: BorderRadius.circular(12),
              ),
              child: Icon(
                Icons.devices,
                color: provider.isConnected ? Colors.green : Colors.grey,
                size: 32,
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    provider.isConnected ? 'veea-device' : 'No Device',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                  Text(
                    provider.isConnected 
                        ? 'Ready to capture'
                        : 'Tap Connect to find devices',
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ],
              ),
            ),
            if (provider.isConnected)
              TextButton(
                onPressed: provider.disconnect,
                child: const Text('Disconnect'),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildEmptyState() {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(
            Icons.photo_library_outlined,
            size: 80,
            color: Colors.grey.shade400,
          ),
          const SizedBox(height: 16),
          Text(
            'No images yet',
            style: TextStyle(
              fontSize: 18,
              color: Colors.grey.shade600,
            ),
          ),
          const SizedBox(height: 8),
          Text(
            'Connect to your Veea device and capture some photos!',
            style: TextStyle(
              fontSize: 14,
              color: Colors.grey.shade500,
            ),
            textAlign: TextAlign.center,
          ),
        ],
      ),
    );
  }
}
