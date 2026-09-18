#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>


// Флаг, который показывает, что программу попросили завершиться
// Его меняет обработчик SIGINT / SIGTERM
volatile sig_atomic_t stopRequested = 0;


// Обработчик сигнала.
// Например, сюда попадём после Ctrl+C.

void handleSignal(int)
{
    stopRequested = 1;
}


// Настраиваем обработку сигналов завершения

bool setupSignalHandlers()
{
    struct sigaction action {};

    action.sa_handler = handleSignal;

    // во время выполнения обработчика не блокируем дополнительные сигналы
    sigemptyset(&action.sa_mask);

    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, nullptr) == -1)
    {
        return false;
    }

    if (sigaction(SIGTERM, &action, nullptr) == -1)
    {
        return false;
    }

    return true;
}


// Переводит строку с HEX-числом в обычное число

// Возвращает true, если строка успешно разобрана
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


// Определение nodeID устройства по строке candump

// Возвращает true, если удалось определить устройство
bool extractNodeId(
    const std::string& line,
    unsigned int& nodeId,
    bool& broadcast)
{
    broadcast = false;

    // Разбиваем строку на отдельные части по пробелам.
    std::istringstream stream(line);
    std::vector<std::string> tokens;

    std::string token;

    while (stream >> token)
    {
        tokens.push_back(token);
    }


    // Здесь будем хранить номер элемента, в котором находится CAN ID
    std::size_t idIndex = tokens.size();


    // Ищем CAN ID

    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        // стандартный CAN ID имеет 3 символа, расширенный из задания - 8 символов
        bool validLength =
            tokens[i].size() == 3 ||
            tokens[i].size() == 8;

        // проверяем, что следующий элемент похож на [8], [1] и т.п.
        bool nextIsLength =
            !tokens[i + 1].empty() &&
            tokens[i + 1].front() == '[';

        if (validLength && nextIsLength)
        {
            idIndex = i;
            break;
        }
    }


    // CAN ID не нашли — строка не обрабатывается
    if (idIndex == tokens.size())
    {
        return false;
    }


    const std::string& canAddress = tokens[idIndex];


    // Расширенная адресация

    if (canAddress.size() == 8)
    {
        // расширенный пакет должен начинаться с 1E0
        if (canAddress.substr(0, 3) != "1E0")
        {
            return false;
        }

        // направление должно быть только 0 или 1
        if (canAddress[3] != '0' &&
            canAddress[3] != '1')
        {
            return false;
        }

        unsigned long value = 0;

        // берутся два символа с nodeID
        if (!parseHex(canAddress.substr(4, 2), value))
        {
            return false;
        }

        // допустимый nodeID: 01 ... 7F
        if (value < 0x01 || value > 0x7F)
        {
            return false;
        }

        nodeId = static_cast<unsigned int>(value);

        return true;
    }


    // Стандартная 11-битная CAN-адресация

    unsigned long canId = 0;

    if (!parseHex(canAddress, canId))
    {
        return false;
    }

    // максимальный стандартный CAN ID — 0x7FF
    if (canId > 0x7FF)
    {
        return false;
    }


    // NMT обрабатываем отдельно
    
    // У NMT CAN ID всегда 000
    

    if (canId == 0x000)
    {
        // проверка, что в строке действительно есть нужные данные
        if (idIndex + 3 >= tokens.size())
        {
            return false;
        }

        unsigned long value = 0;

        // берём nodeID из второго байта данных
        if (!parseHex(tokens[idIndex + 3], value))
        {
            return false;
        }

        // nodeID 00 у NMT означает команду всем устройствам.
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


    // для обычных CANopen-сообщений nodeID находится в младших 7 битах CAN ID

    unsigned int value =
        static_cast<unsigned int>(canId & 0x7F);


    // нулевой nodeID не относится к конкретному устройству
    if (value == 0)
    {
        return false;
    }


    nodeId = value;

    return true;
}

// Вывод -h (help)
void printHelp(const char* programName)
{
    std::cout
        << "CanUdpFilter - фильтрация CAN/CANopen сообщений и отправка подходящих строк на UDP-сервер\n\n"

        << "Использование:\n"
        << "  " << programName
        << " <server> <port> <nodeID> [nodeID ...]\n\n"

        << "Параметры:\n"
        << "  server   IP-адрес или имя UDP-сервера\n"
        << "  port     UDP-порт (1-65535)\n"
        << "  nodeID   CANopen nodeID в HEX, диапазон 01-7F\n\n"

        << "Пример:\n"
        << "  candump can0 | "
        << programName
        << " 127.0.0.1 5000 04 06 07\n\n"

        << "Справка:\n"
        << "  " << programName << " -h\n"
        << "  " << programName << " --help\n";
}

int main(int argc, char* argv[])
{
    // Формат запуска:
    // CanUdpFilter <server> <port> <nodeID> [nodeID ...]
    //
    // Например:
    // CanUdpFilter 127.0.0.1 5000 04 06 07

    if (argc == 2 &&
        (std::string(argv[1]) == "-h" ||
            std::string(argv[1]) == "--help"))
    {
        printHelp(argv[0]);
        return 0;
    }

    // сперва включаем обработку SIGINT и SIGTERM
    if (!setupSignalHandlers())
    {
        perror("sigaction");
        return 1;
    }


    if (argc < 4)
    {
        std::cerr
            << "Правильный формат: CanUdpFilter <server> <port> "
            "<nodeID> [nodeID ...]\n"
            << "Пример: CanUdpFilter 127.0.0.1 5000 04 06 07\n";
        printHelp(argv[0]);

        return 1;
    }


    // первый аргумент - адрес UDP-сервера
    const std::string serverAddress = argv[1];

    // второй аргумент - UDP-порт
    const std::string serverPort = argv[2];



    // проверяем UDP-порт

    try
    {
        std::size_t pos = 0;

        unsigned long port =
            std::stoul(serverPort, &pos, 10);

        // допустимые порты: 1 ... 65535
        if (pos != serverPort.size() ||
            port == 0 ||
            port > 65535)
        {
            std::cerr
                << "Неверный UDP port: "
                << serverPort
                << '\n';

            return 1;
        }
    }
    catch (...)
    {
        std::cerr
            << "Неверный UDP port: "
            << serverPort
            << '\n';

        return 1;
    }



    // читаем список nodeID из аргументов программы


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

        // set автоматически убирает повторяющиеся значения
        targetNodeIds.insert(
            static_cast<unsigned int>(value));
    }



    // Получение сетевого адреса UDP-сервера


    addrinfo hints{};

    // Поддержка IPv4 и IPv6
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



    //UDP-сокет

    int socketFd = -1;
    addrinfo* destination = nullptr;


    // создаётся сокет, пока не найдётся рабочий
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
        std::cerr
            << "Невозможно создать UDP socket\n";

        freeaddrinfo(addresses);

        return 1;
    }


  
  
    // Чтение потока по строкам


    std::string line;


    while (!stopRequested)
    {
        // Читаем по строке
        if (!std::getline(std::cin, line))
        {
            // Если пришёл SIGINT / SIGTERM, то завершаемся
            if (stopRequested)
            {
                break;
            }

            // Если входной поток закончился, то также завершение
            if (std::cin.eof())
            {
                break;
            }

            std::cerr
                << "Ошибка чтения.\n";

            break;
        }


        unsigned int nodeId = 0;
        bool broadcast = false;


        // Определяем, какому устройству принадлежит строка
        if (!extractNodeId(line, nodeId, broadcast))
        {
            continue;
        }

        bool shouldSend =
            broadcast ||
            targetNodeIds.count(nodeId) != 0;


        // Чужое устройство - пропускаем строки
        if (!shouldSend)
        {
            continue;
        }


        // Отправка по UDP исходной строки

        ssize_t sent = sendto(
            socketFd,
            line.data(),
            line.size(),
            0,
            destination->ai_addr,
            destination->ai_addrlen);


        // sendto вернул ошибку
        if (sent < 0)
        {
            // Если отправка была прервана сигналом завершения, значит не ошибка
            if (errno == EINTR && stopRequested)
            {
                break;
            }

            perror("sendto");

            close(socketFd);
            freeaddrinfo(addresses);

            return 2;
        }


        // Для UDP ожидаем, что отправлена вся датаграмма
        if (static_cast<std::size_t>(sent) != line.size())
        {
            std::cerr
                << "UDP датаграмма не была отправлена полностью\n";

            close(socketFd);
            freeaddrinfo(addresses);

            return 2;
        }
    }


    // Освобождение ресурсов перед завершением


    close(socketFd);
    freeaddrinfo(addresses);


    if (stopRequested)
    {
        std::cerr
            << "\nПолучен сигнал завершения. "
            "Программа остановлена.\n";
    }


    return 0;
}
