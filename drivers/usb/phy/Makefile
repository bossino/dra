#
# Makefile for physical layer USB drivers
#

ccflags-$(CONFIG_USB_DEBUG) := -DDEBUG

obj-$(CONFIG_OMAP_USB2)			+= omap-usb2.o
obj-$(CONFIG_OMAP_USB3)			+= omap-usb3.o
obj-$(CONFIG_OMAP_CONTROL_USB)		+= omap-control-usb.o
obj-$(CONFIG_USB_ISP1301)		+= isp1301.o
obj-$(CONFIG_MV_U3D_PHY)		+= mv_u3d_phy.o
obj-$(CONFIG_USB_EHCI_TEGRA)	+= tegra_usb_phy.o
obj-$(CONFIG_USB_RCAR_PHY)		+= rcar-phy.o
obj-$(CONFIG_OF)			+= of.o

