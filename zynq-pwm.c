#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/io.h>

#define AXI_PWM_GENERATE_UP_ADDR    0x00000000
#define AXI_PWM_GENERATE_DOWN_ADDR  0x00000000
#define AXI_PWM_CAPTURE_UP_ADDR     0x00000000
#define AXI_PWM_CAPTURE_DOWN_ADDR   0x00000000

struct axi_pwm_chip {
    struct device *dev;
    struct pwm_chip chip;
    void __iomem *base_addr;
    int clk_ns;
};

static inline struct axi_pwm_chip *to_axi_pwm_chip(struct pwm_chip *chip){
    return container_of(chip, struct axi_pwm_chip, chip);
}

// configure output duty_cycle and frequency
static int axi_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm, int duty_ns, int period_ns){
    struct axi_pwm_chip *pc;
    pc = container_of(chip, struct axi_pwm_chip, chip);
    u32 up_count = pc->clk_ns / duty_ns;
    u32 down_count = pc->clk_ns / (pc->clk_ns - duty_ns);
    iowrite32(up_count, pc->base_addr + AXI_PWM_CAPTURE_UP_ADDR);
    iowrite32(down_count, pc->base_addr + AXI_PWM_CAPTURE_DOWN_ADDR);
    return 0;
}

static int axi_pwm_capture(struct pwm_chip *chip, struct pwm_device *pwm, struct pwm_capture *result, unsigned long timeout){
    // TODO: we don't provide timeout, the result only depend on late capture time.
    // slide average should be apply here for timeout.
    struct axi_pwm_chip *pc;
    pc = container_of(chip, struct axi_pwm_chip, chip);
    u32 up_count = ioread32(pc->base_addr + AXI_PWM_CAPTURE_UP_ADDR);
    u32 down_count = ioread32(pc->base_addr + AXI_PWM_CAPTURE_DOWN_ADDR);
    result->period = pc->clk_ns / (up_count + down_count);
    result->duty_cycle = pc->clk_ns / up_count; 
    return 0;
}

static int axi_pwm_remove(struct platform_device *pdev) {
    struct axi_pwm_chip *axi_pwm = platform_get_drvdata(pdev);
    clk_disable_unprepare(axi_pwm->clk);
    return pwmchip_remove(&axi_pwm->chip);
}

static const struct pwm_ops axi_pwm_ops = {
    .config = axi_pwm_config,
    .capture = axi_pwm_capture,
    .owner = THIS_MODULE,
};

struct axi_pwm_data {
    unsigned int duty_event;
};

static const struct of_device_id axi_pwm_of_match[] = {
    { .compatible = "axi-pwm" },
    {}
};
MODULE_DEVICE_TABLE(of, axi_pwm_of_match);

static int axi_pwm_probe(struct platform_device *pdev) {
    struct axi_pwm_chip *axi_pwm;
    struct pwm_device *pwm;
    struct resource *res;
    struct clk *clk;
    int ret,i;
    // allocate memory
    axi_pwm = devm_kzalloc(&pdev->dev, sizeof(*axi_pwm), GFP_KERNEL);
    if (!axi_pwm)
        return -ENOMEM;
    // get the base addr to res
    axi_pwm->dev = &pdev->dev;
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) 
        return -ENODEV;
    axi_pwm->base_addr = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(axi_pwm->base_addr))
        return PTR_ERR(axi_pwm->base_addr);
    // get axi clock
    clk = devm_clk_get(&pdev->dev, NULL);
    if (IS_ERR(clk)) {
        dev_err(&pdev->dev, "failed to get pwm clock\n");
        return PTR_ERR(clk);
    }
    axi_pwm->clk_ns = (int)1000000000/clk_get_rate(clk);
    // pwm_chip settings binding
    axi_pwm->chip.dev = &pdev->dev;
    axi_pwm->chip.ops = &axi_pwm_ops;
    axi_pwm->chip.base = -1;
    axi_pwm->chip.npwm = 1;
    axi_pwm->chip.of_xlate = &of_pwm_xlate_with_flags;
    axi_pwm->chip.of_pwm_n_cells = 1;
    ret = pwmchip_add(&axi_pwm->chip);
    if (ret < 0) {
        dev_err(&pdev->dev, "pwmchip_add failed: %d\n", ret);
        goto disable_pwmclk;
     }   
    // add each pwm device
    for (i = 0; i < axi_pwm->chip.npwm; i++) {
            struct axi_pwm_data *data;
            pwm = &axi_pwm->chip.pwms[i];

            data = devm_kzalloc(axi_pwm->dev, sizeof(*data), GFP_KERNEL);
            if (!data) {
                ret = -ENOMEM;
                goto pwmchip_remove;
            }
            pwm_set_chip_data(pwm, data);
    }
    // after finish all task below, set driver data to platform.
    platform_set_drvdata(pdev, pwm);
    return 0;
    
    pwmchip_remove:
    pwmchip_remove(&axi_pwm->chip);
    disable_pwmclk:
    clk_disable_unprepare(clk);
    return ret;
}

static struct platform_driver axi_pwm_driver = {
    .driver = {
        .name = "axi-pwm",
        .owner = THIS_MODULE,
        .of_match_table = axi_pwm_of_match,
    },
    .probe = axi_pwm_probe,
    .remove = axi_pwm_remove,
};
module_platform_driver(axi_pwm_driver);

MODULE_LICENSE("GPL"); 
MODULE_DESCRIPTION("A AXI PWM driver");
