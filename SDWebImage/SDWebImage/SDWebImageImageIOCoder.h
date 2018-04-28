/*
 * This file is part of the SDWebImage package.
 * (c) Olivier Poitrey <rs@dailymotion.com>
 *
 * For the full copyright and license information, please view the LICENSE
 * file that was distributed with this source code.
 */

#import <Foundation/Foundation.h>
#import "SDWebImageCoder.h"

/**
 Built in coder that supports PNG, JPEG, TIFF, includes support for progressive decoding.
 
 GIF
 Also supports static GIF (meaning will only handle the 1st frame).
 For a full GIF support, we recommend `FLAnimatedImage` or our less performant `SDWebImageGIFCoder`
 
 HEIC
 This coder also supports HEIC format because ImageIO supports it natively. But it depends on the system capabilities, so it won't work on all devices, see: https://devstreaming-cdn.apple.com/videos/wwdc/2017/511tj33587vdhds/511/511_working_with_heif_and_hevc.pdf
 Decode(Software): !Simulator && (iOS 11 || tvOS 11 || macOS 10.13)
 Decode(Hardware): !Simulator && ((iOS 11 && A9Chip) || (macOS 10.13 && 6thGenerationIntelCPU))
 Encode(Software): macOS 10.13
 Encode(Hardware): !Simulator && ((iOS 11 && A10FusionChip) || (macOS 10.13 && 6thGenerationIntelCPU))
 并不是所有的设备都支持 HEIC 编解码
 安装了 macOS 10.13 或者 iOS 11的设备都能支持 HEIF 的解码，其中 A9 芯片以上的 iOS 设备
 （iPhone6s 或 iPad Pro）和 6 代因特尔内核的 macOS 设备（the new MacBook with the touch bar）
 支持硬件解码。
 */
@interface SDWebImageImageIOCoder : NSObject <SDWebImageProgressiveCoder>

+ (nonnull instancetype)sharedCoder;

@end
