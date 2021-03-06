/*
 * omap-usb3 - USB PHY, talking to dwc3 controller in OMAP.
 *
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/usb/omap_usb.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/usb/omap_control_usb.h>
#include <linux/of_platform.h>

#define	NUM_SYS_CLKS		6
#define	PLL_STATUS		0x00000004
#define	PLL_GO			0x00000008
#define	PLL_CONFIGURATION1	0x0000000C
#define	PLL_CONFIGURATION2	0x00000010
#define	PLL_CONFIGURATION3	0x00000014
#define	PLL_CONFIGURATION4	0x00000020

#define	PLL_REGM_MASK		0x001FFE00
#define	PLL_REGM_SHIFT		0x9
#define	PLL_REGM_F_MASK		0x0003FFFF
#define	PLL_REGM_F_SHIFT	0x0
#define	PLL_REGN_MASK		0x000001FE
#define	PLL_REGN_SHIFT		0x1
#define	PLL_SELFREQDCO_MASK	0x0000000E
#define	PLL_SELFREQDCO_SHIFT	0x1
#define	PLL_SD_MASK		0x0003FC00
#define	PLL_SD_SHIFT		0x9
#define	SET_PLL_GO		0x1
#define	PLL_TICOPWDN		0x10000
#define	PLL_LOCK		0x2
#define	PLL_IDLE		0x1

/*
 * This is an Empirical value that works, need to confirm the actual
 * value required for the USB3PHY_PLL_CONFIGURATION2.PLL_IDLE status
 * to be correctly reflected in the USB3PHY_PLL_STATUS register.
 */
# define PLL_IDLE_TIME  100;

enum sys_clk_rate {
	CLK_RATE_UNDEFINED = -1,
	CLK_RATE_12MHZ,
	CLK_RATE_16MHZ,
	CLK_RATE_19MHZ,
	CLK_RATE_20MHZ,
	CLK_RATE_26MHZ,
	CLK_RATE_38MHZ
};

static struct usb_dpll_params omap_usb3_dpll_params[NUM_SYS_CLKS] = {
	{1250, 5, 4, 20, 0},		/* 12 MHz */
	{3125, 20, 4, 20, 0},		/* 16.8 MHz */
	{1172, 8, 4, 20, 65537},	/* 19.2 MHz */
	{1000, 7, 4, 10, 0},		/* 20 MHz */
	{1250, 12, 4, 20, 0},		/* 26 MHz */
	{3125, 47, 4, 20, 92843},	/* 38.4 MHz */
};

static int omap_usb3_suspend(struct usb_phy *x, int suspend)
{
	struct omap_usb *phy = phy_to_omapusb(x);
	int	val;
	int timeout = PLL_IDLE_TIME;

	if (suspend && !phy->is_suspended) {
		val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION2);
		val |= PLL_IDLE;
		omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION2, val);

		do {
			val = omap_usb_readl(phy->pll_ctrl_base, PLL_STATUS);
			if (val & PLL_TICOPWDN)
				break;
			udelay(1);
		} while (--timeout);

		omap_control_usb3_phy_power(phy->control_dev, 0);

		phy->is_suspended	= 1;
	} else if (!suspend && phy->is_suspended) {
		phy->is_suspended	= 0;

		val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION2);
		val &= ~PLL_IDLE;
		omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION2, val);

		do {
			val = omap_usb_readl(phy->pll_ctrl_base, PLL_STATUS);
			if (!(val & PLL_TICOPWDN))
				break;
			udelay(1);
		} while (--timeout);
	}

	return 0;
}

static inline enum sys_clk_rate __get_sys_clk_index(unsigned long rate)
{
	switch (rate) {
	case 12000000:
		return CLK_RATE_12MHZ;
	case 16800000:
		return CLK_RATE_16MHZ;
	case 19200000:
		return CLK_RATE_19MHZ;
	case 20000000:
		return CLK_RATE_20MHZ;
	case 26000000:
		return CLK_RATE_26MHZ;
	case 38400000:
		return CLK_RATE_38MHZ;
	default:
		return CLK_RATE_UNDEFINED;
	}
}

static void omap_usb_dpll_relock(struct omap_usb *phy)
{
	u32		val;
	unsigned long	timeout;

	omap_usb_writel(phy->pll_ctrl_base, PLL_GO, SET_PLL_GO);

	timeout = jiffies + msecs_to_jiffies(20);
	do {
		val = omap_usb_readl(phy->pll_ctrl_base, PLL_STATUS);
		if (val & PLL_LOCK)
			break;
	} while (!WARN_ON(time_after(jiffies, timeout)));
}

static int omap_usb_dpll_lock(struct omap_usb *phy)
{
	u32			val;
	unsigned long		rate;
	enum sys_clk_rate	clk_index;

	rate		= clk_get_rate(phy->sys_clk);
	clk_index	= __get_sys_clk_index(rate);

	if (clk_index == CLK_RATE_UNDEFINED) {
		pr_err("dpll cannot be locked for sys clk freq:%luHz\n", rate);
		return -EINVAL;
	}

	val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION1);
	val &= ~PLL_REGN_MASK;
	val |= omap_usb3_dpll_params[clk_index].n << PLL_REGN_SHIFT;
	omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION1, val);

	val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION2);
	val &= ~PLL_SELFREQDCO_MASK;
	val |= omap_usb3_dpll_params[clk_index].freq << PLL_SELFREQDCO_SHIFT;
	omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION2, val);

	val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION1);
	val &= ~PLL_REGM_MASK;
	val |= omap_usb3_dpll_params[clk_index].m << PLL_REGM_SHIFT;
	omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION1, val);

	val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION4);
	val &= ~PLL_REGM_F_MASK;
	val |= omap_usb3_dpll_params[clk_index].mf << PLL_REGM_F_SHIFT;
	omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION4, val);

	val = omap_usb_readl(phy->pll_ctrl_base, PLL_CONFIGURATION3);
	val &= ~PLL_SD_MASK;
	val |= omap_usb3_dpll_params[clk_index].sd << PLL_SD_SHIFT;
	omap_usb_writel(phy->pll_ctrl_base, PLL_CONFIGURATION3, val);

	omap_usb_dpll_relock(phy);

	return 0;
}

static int omap_usb3_init(struct usb_phy *x)
{
	struct omap_usb	*phy = phy_to_omapusb(x);

	omap_usb_dpll_lock(phy);
	omap_control_usb3_phy_power(phy->control_dev, 1);

	return 0;
}

static int omap_usb3_probe(struct platform_device *pdev)
{
	struct omap_usb			*phy;
	struct resource			*res;
	struct device_node		*node = pdev->dev.of_node;
	struct device_node		*omap_control_usb_node;
	struct platform_device		*pdev_control_usb;
	const char   			*clk_name;

	phy = devm_kzalloc(&pdev->dev, sizeof(*phy), GFP_KERNEL);
	if (!phy) {
		dev_err(&pdev->dev, "unable to alloc mem for OMAP USB3 PHY\n");
		return -ENOMEM;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pll_ctrl");
	phy->pll_ctrl_base = devm_request_and_ioremap(&pdev->dev, res);
	if (!phy->pll_ctrl_base) {
		dev_err(&pdev->dev, "ioremap of pll_ctrl failed\n");
		return -ENOMEM;
	}

	phy->dev		= &pdev->dev;
	phy->phy.dev		= phy->dev;
	phy->phy.label		= "omap-usb3";
	phy->phy.init		= omap_usb3_init;
	phy->phy.set_suspend	= omap_usb3_suspend;
	phy->phy.type		= USB_PHY_TYPE_USB3;
	phy->is_suspended	= 1;

	of_property_read_string(node, "wkupclk", &clk_name);
	if (!clk_name) {
		dev_err(&pdev->dev, "unable to read wkupclk property from dt \n");
		return -EINVAL;
	} else {
		phy->wkupclk = devm_clk_get(phy->dev, clk_name);
		if (IS_ERR(phy->wkupclk)) {
			dev_err(&pdev->dev, "unable to get usb_phy wk clk\n");
			return PTR_ERR(phy->wkupclk);
		}
	}

	clk_prepare(phy->wkupclk);

	of_property_read_string(node, "optclk", &clk_name);
	if (!clk_name)
		dev_err(&pdev->dev, "unable to read optclk property from dt\n");
	else {
		phy->optclk = devm_clk_get(phy->dev, clk_name);
		if (IS_ERR(phy->optclk)) {
			dev_err(&pdev->dev, "unable to get usb_phy opt clk\n");
			return PTR_ERR(phy->optclk);
		}
		else
			clk_prepare(phy->optclk);
	}


	phy->sys_clk = devm_clk_get(phy->dev, "sys_clkin");
	if (IS_ERR(phy->sys_clk)) {
		pr_err("%s: unable to get sys_clkin\n", __func__);
		return -EINVAL;
	}

	omap_control_usb_node   = of_parse_phandle(node, "ctrl-module", 0);
	if (IS_ERR(omap_control_usb_node)) {
		dev_err(&pdev->dev, "Failed to find ctrl-module\n");
		return -EPROBE_DEFER;
	}

	pdev_control_usb = of_find_device_by_node(omap_control_usb_node);
	if (IS_ERR(pdev_control_usb)) {
		dev_dbg(&pdev->dev, "Attempt to get the platform control usb failed\n");
		return -EPROBE_DEFER;
	}

	phy->control_dev = &pdev_control_usb->dev;
	if (IS_ERR(phy->control_dev)) {
		dev_dbg(&pdev->dev, "Failed to get control device\n");
		return -ENODEV;
	}

	dev_dbg(&pdev->dev, "got control usb name %s\n",
					dev_name(phy->control_dev));
	phy->control_node = omap_control_usb_node;

	omap_control_usb3_phy_power(phy->control_dev, 0);
	usb_add_phy_dev(&phy->phy);

	platform_set_drvdata(pdev, phy);

	pm_runtime_enable(phy->dev);
	pm_runtime_get(&pdev->dev);

	return 0;
}

static int omap_usb3_remove(struct platform_device *pdev)
{
	struct omap_usb *phy = platform_get_drvdata(pdev);

	clk_unprepare(phy->wkupclk);
	clk_unprepare(phy->optclk);
	of_node_put(phy->control_node);
	usb_remove_phy(&phy->phy);
	if (!pm_runtime_suspended(&pdev->dev))
		pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM_RUNTIME

static int omap_usb3_runtime_suspend(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct omap_usb	*phy = platform_get_drvdata(pdev);

	clk_disable(phy->wkupclk);
	clk_disable(phy->optclk);

	return 0;
}

static int omap_usb3_runtime_resume(struct device *dev)
{
	u32 ret = 0;
	struct platform_device	*pdev = to_platform_device(dev);
	struct omap_usb	*phy = platform_get_drvdata(pdev);

	ret = clk_enable(phy->optclk);
	if (ret) {
		dev_err(phy->dev, "Failed to enable optclk %d\n", ret);
		goto err1;
	}

	ret = clk_enable(phy->wkupclk);
	if (ret) {
		dev_err(phy->dev, "Failed to enable wkupclk %d\n", ret);
		goto err2;
	}

	return 0;

err2:
	clk_disable(phy->optclk);

err1:
	return ret;
}

static const struct dev_pm_ops omap_usb3_pm_ops = {
	SET_RUNTIME_PM_OPS(omap_usb3_runtime_suspend, omap_usb3_runtime_resume,
		NULL)
};

#define DEV_PM_OPS     (&omap_usb3_pm_ops)
#else
#define DEV_PM_OPS     NULL
#endif

#ifdef CONFIG_OF
static const struct of_device_id omap_usb3_id_table[] = {
	{ .compatible = "ti,omap-usb3" },
	{}
};
MODULE_DEVICE_TABLE(of, omap_usb3_id_table);
#endif

static struct platform_driver omap_usb3_driver = {
	.probe		= omap_usb3_probe,
	.remove		= omap_usb3_remove,
	.driver		= {
		.name	= "omap-usb3",
		.owner	= THIS_MODULE,
		.pm	= DEV_PM_OPS,
		.of_match_table = of_match_ptr(omap_usb3_id_table),
	},
};

module_platform_driver(omap_usb3_driver);

MODULE_ALIAS("platform: omap_usb3");
MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("OMAP USB3 phy driver");
MODULE_LICENSE("GPL v2");
