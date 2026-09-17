#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>


bool parseHex(const std::string& text, unsigned long& value)
{
    try
    {
        std::size_t pos = 0;

        value = std::stoul(text, &pos, 16);

        return pos == text.size();
    }
    catch (...)
    {
        return false;
    }
}


bool extractNodeId(
    const std::string& line,
    unsigned int& nodeId,
    bool& broadcast)
{
    broadcast = false;

    std::istringstream stream(line);
    std::vector<std::string> tokens;

    std::string token;

    while (stream >> token)
    {
        tokens.push_back(token);
    }

    std::size_t idIndex = tokens.size();

    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        bool validLength =
            tokens[i].size() == 3 ||
            tokens[i].size() == 8;

        bool nextIsLength =
            !tokens[i + 1].empty() &&
            tokens[i + 1].front() == '[';

        if (validLength && nextIsLength)
        {
            idIndex = i;
            break;
        }
    }

    if (idIndex == tokens.size())
    {
        return false;
    }

    const std::string& canAddress = tokens[idIndex];


    if (canAddress.size() == 8)
    {
        if (canAddress.substr(0, 3) != "1E0")
        {
            return false;
        }

        if (canAddress[3] != '0' &&
            canAddress[3] != '1')
        {
            return false;
        }

        unsigned long value = 0;

        if (!parseHex(canAddress.substr(4, 2), value))
        {
            return false;
        }

        if (value < 0x01 || value > 0x7F)
        {
            return false;
        }

        nodeId = static_cast<unsigned int>(value);

        return true;
    }

    unsigned long canId = 0;

    if (!parseHex(canAddress, canId))
    {
        return false;
    }

    if (canId > 0x7FF)
    {
        return false;
    }

    // NMT

    if (canId == 0x000)
    {
        if (idIndex + 3 >= tokens.size())
        {
            return false;
        }

        unsigned long value = 0;

        if (!parseHex(tokens[idIndex + 3], value))
        {
            return false;
        }

        if (value == 0x00)
        {
            broadcast = true;
            return true;
        }

        if (value > 0x7F)
        {
            return false;
        }

        nodeId = static_cast<unsigned int>(value);

        return true;
    }

    unsigned int value =
        static_cast<unsigned int>(canId & 0x7F);

    if (value == 0)
    {
        return false;
    }

    nodeId = value;

    return true;
}

int main(int argc, char* argv[])
{
    // Формат:
    // CanUdpFilter <server> <port> <nodeID> [nodeID ...]
    //
    // Например:
    // CanUdpFilter 127.0.0.1 5000 04 06 07

    if (argc < 4)
    {
        std::cerr
            << "Правильный формат: CanUdpFilter <server> <port> "
            "<nodeID> [nodeID ...]\n"
            << "Пример: CanUdpFilter 127.0.0.1 5000 04 06 07\n";

        return 1;
    }

    const std::string serverAddress = argv[1];
    const std::string serverPort = argv[2];


    try
    {
        std::size_t pos = 0;

        unsigned long port =
            std::stoul(serverPort, &pos, 10);

        if (pos != serverPort.size() ||
            port == 0 ||
            port > 65535)
        {
            std::cerr << "Неверный UDP port: "
                << serverPort << '\n';

            return 1;
        }
    }
    catch (...)
    {
        std::cerr << "Неверный UDP port: "
            << serverPort << '\n';

        return 1;
    }


    std::set<unsigned int> targetNodeIds;

    for (int i = 3; i < argc; ++i)
    {
        unsigned long value = 0;

        if (!parseHex(argv[i], value) ||
            value < 0x01 ||
            value > 0x7F)
        {
            std::cerr
                << "Неверный nodeID: "
                << argv[i]
                << '\n';

            return 1;
        }

        targetNodeIds.insert(
            static_cast<unsigned int>(value));
    }


    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* addresses = nullptr;

    int result = getaddrinfo(
        serverAddress.c_str(),
        serverPort.c_str(),
        &hints,
        &addresses);

    if (result != 0)
    {
        std::cerr
            << "getaddrinfo failed: "
            << gai_strerror(result)
            << '\n';

        return 1;
    }


    int socketFd = -1;
    addrinfo* destination = nullptr;

    for (addrinfo* addr = addresses;
        addr != nullptr;
        addr = addr->ai_next)
    {
        socketFd = socket(
            addr->ai_family,
            addr->ai_socktype,
            addr->ai_protocol);

        if (socketFd >= 0)
        {
            destination = addr;
            break;
        }
    }

    if (socketFd < 0 || destination == nullptr)
    {
        std::cerr << "Невозможно создать UDP socket\n";

        freeaddrinfo(addresses);

        return 1;
    }


    std::string line;

    while (std::getline(std::cin, line))
    {
        unsigned int nodeId = 0;
        bool broadcast = false;

        if (!extractNodeId(line, nodeId, broadcast))
        {
            continue;
        }

        bool shouldSend =
            broadcast ||
            targetNodeIds.count(nodeId) != 0;

        if (!shouldSend)
        {
            continue;
        }

        ssize_t sent = sendto(
            socketFd,
            line.data(),
            line.size(),
            0,
            destination->ai_addr,
            destination->ai_addrlen);

        if (sent < 0)
        {
            perror("sendto");

            close(socketFd);
            freeaddrinfo(addresses);

            return 2;
        }

        if (static_cast<std::size_t>(sent) != line.size())
        {
            std::cerr
                << "UDP датаграмма не была отправлена полностью\n";

            close(socketFd);
            freeaddrinfo(addresses);

            return 2;
        }
    }


    close(socketFd);
    freeaddrinfo(addresses);

    return 0;
}
