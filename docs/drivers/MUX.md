# MUX

MUX is a small custom driver class for selecting channels on a multiplexer.

It exists so KScan can support different mux hardware through one API.

## Public API

```c
int mux_select(const struct device *dev, unsigned int channel);
int mux_select_next(const struct device *dev);
int mux_get_current_channel(const struct device *dev);
int mux_get_channel_amount(const struct device *dev);
int mux_enable(const struct device *dev);
int mux_disable(const struct device *dev);
bool mux_is_enabled(const struct device *dev);
```

## GPIO mux

The current implementation is `mux-gpio`.

Example:

```dts
mux1: mux1 {
    compatible = "mux-gpio";
    sel-gpios = <&gpio0 12 GPIO_ACTIVE_HIGH>,
                <&gpio0 11 GPIO_ACTIVE_HIGH>,
                <&gpio0 10 GPIO_ACTIVE_HIGH>;
    channels = <8>;
    #mux-cells = <0>;
};
```

`sel-gpios` are binary select lines.

`channels` is the number of valid selected channels.

## Usage

KScan MUXes uses this driver to select one sensor channel, wait for optional settle time, then read the ADC.

Do not put keyboard policy here. The mux driver should only select channels and track enabled/current state.

## Important Kconfig

- `CONFIG_MUX`
- `CONFIG_MUX_GPIO`
- `CONFIG_MUX_GPIO_MAX_SEL_CNT`
