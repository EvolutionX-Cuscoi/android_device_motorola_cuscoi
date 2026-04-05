#
# Copyright (C) 2025 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# AAPT
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := 400dpi
PRODUCT_AAPT_PREBUILT_DPI := xxxhdpi xxhdpi xhdpi hdpi

# Boot animation
TARGET_SCREEN_HEIGHT := 2400
TARGET_SCREEN_WIDTH := 1080

# Camera Extensions
$(call soong_config_set,libcameraservice,ext_lib,//$(LOCAL_PATH):libcameraservice_extension.cuscoi)

# Init
$(foreach f,$(wildcard $(LOCAL_PATH)/rootdir/etc/init/hw/*.rc),\
        $(eval PRODUCT_COPY_FILES += $(f):$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/$(notdir $f)))
$(foreach f,$(wildcard $(LOCAL_PATH)/rootdir/etc/init/*.rc),\
        $(eval PRODUCT_COPY_FILES += $(f):$(TARGET_COPY_OUT_VENDOR)/etc/init/$(notdir $f)))
$(foreach f,$(wildcard $(LOCAL_PATH)/rootdir/bin/*.sh),\
        $(eval PRODUCT_COPY_FILES += $(f):$(TARGET_COPY_OUT_VENDOR)/bin/$(notdir $f)))

# NFC
TARGET_USES_ST_NFC := true

# Overlay
PRODUCT_PACKAGES += \
    FrameworksResDevice \
    LineageSystemUIDevice \
    SettingsResDevice \
    SystemUIResDevice \
    WifiResDevice \
    LauncherOverlay

# DeviceAsWebcam
PRODUCT_PACKAGES += \
    DeviceAsWebcam

PRODUCT_PACKAGES += \
    RemovePackages

# Screen
TARGET_SCREEN_DENSITY := 400

COMMA := ,

#PRODUCT_PACKAGES += \
#    vendor.lineage.livedisplay-service.sdm \
#    vendor.lineage.livedisplay-service.sysfs

#$(call soong_config_set_bool,livedisplay_sdm,enable_dm,false)
#$(call soong_config_set_bool,livedisplay_sysfs,enable_af,true)
#$(call soong_config_set, livedisplay_sysfs, af_path, /sys/devices/platform/soc/ae00000.qcom$(COMMA)mdss_mdp/drm/card0/card0-DSI-1/dc_dimming)
#$(call soong_config_set_bool,livedisplay_sysfs,enable_se,true)
#$(call soong_config_set, livedisplay_sysfs, se_path, /sys/devices/platform/soc/ae00000.qcom$(COMMA)mdss_mdp/drm/card0/card0-DSI-1/hbm)

$(call soong_config_set,surfaceflinger,udfps_lib,//device/motorola/cuscoi/udfps:libudfps_extension.cuscoi)

# Shipping API Level
BOARD_SHIPPING_API_LEVEL := 31
PRODUCT_SHIPPING_API_LEVEL := 34

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += $(LOCAL_PATH)

# Wi-Fi
TARGET_WIFI_VARIANTS := adrastea

# Inherit from motorola sm7435-common
$(call inherit-product, device/motorola/sm7435-common/common.mk)

# Inherit from vendor blobs
$(call inherit-product, vendor/motorola/cuscoi/cuscoi-vendor.mk)
