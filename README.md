# Inverter Poller MQTT

This is a simple program designed to query the basic runtime parameters of Voltronic, Axpert, Mppsolar PIP, Voltacon, Effekta, and other branded OEM solar inverters and publis the data on mqtt.

This is a fork from the original Inverter Poller [@manio](https://github.com/manio/skymax-demo).

There is also a home-assistant version ffrom [@ned-kelly](https://github.com/ned-kelly/docker-voltronic-homeassistant) but it requires doker.

On this version i have added mqtt support witch improve reliability, make it lighter (uses less cpu) and reach faster updates times (i use with `run_interval=1`)
 


------------------------------------------------------------

## Compilation / running

build/compilation procedure:
```
git clone git://github.com/greenpixel-it/Inverter-Poller-Mqtt.git
cd Inverter-Poller-Mqtt
mkdir build && cd build
cmake .. && make
```

run/execution procedure:
```
cd build
./inverter_poller &
```

The code requires your inverter to be connected either via USB or RS323, and can be configured in the `inverter.conf` file... 

The code requires also a valid mqtt configuration in the `inverter.conf` file...

You can also place the file on `/etc/inverter/inverter.conf`
