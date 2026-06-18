#!/system/bin/sh
# Create the shared config directory on /sdcard
mkdir -p /sdcard/LudoRD
chmod 777 /sdcard/LudoRD

# Write default config if it doesn't exist
[ -f /sdcard/LudoRD/config.conf ] || cat > /sdcard/LudoRD/config.conf << 'EOF'
force_dice=0
red_always_wins=0
ai_dumb=0
EOF

chmod 666 /sdcard/LudoRD/config.conf
ui_print "- Created /sdcard/LudoRD/config.conf"
