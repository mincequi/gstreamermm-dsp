#include <iostream>

#include <coro/audio/AlsaUtil.h>

int main(void)
{
    coro::AlsaUtil alsaUtil;
    auto outputDevices = alsaUtil.outputDevices();

    for (const auto& dev : outputDevices) {
        std::cout << dev.name << std::endl;
    }

    return 0;
}
