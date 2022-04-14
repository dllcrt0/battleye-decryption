#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <iostream>

void decrypt_generic(uint8_t* data, uint32_t size) {
    for (uint32_t i = size - 1; i >= 4; i--) {
        uint8_t key = ~(data[i - 1]);
        if (i == 4) key = data[0];
        data[i] ^= 0xA5 ^ (i - 4) ^ key;
    }
}

void decrypt_hardware_information_generic(uint8_t* data) {
    for (uint32_t i = 0x16C; i > 0; i--) {
        uint8_t key = ~(data[i - 1]);
        if (i == 1) key = data[0];
        data[i] ^= 0xA5 ^ (i - 1) ^ key;
    }
}

void encrypt_hardware_information_generic(uint8_t* data) {
    uint8_t key = data[0];
    uint8_t index = 0;

    for (uint32_t i = 0; i < 0x16C; i++) {
        uint8_t next_key = index ^ data[i + 1] ^ key ^ 0xA5;
        data[i + 1] = next_key;
        key = ~next_key;
        index = i + 1;
    }
}

void crypt_second_stage(uint8_t* data, int size) {
    int key = *(int*)&data[1];
    data = &data[5];

    bool end = false;
    int index = 0;
    int pre_key = key;
    int post_key = 0;

    while (index + 3 < size) {
        *(uint32_t*)&data[index] ^= pre_key;
        if (end) break;

        post_key = pre_key;
        for (int i = 0; i < (index & 0x8000001F); i++)
            post_key /= 2;

        if (((uint8_t)(post_key) & 1) == 1)
            pre_key = ~key;
        else pre_key = key;

        post_key = 0;
        uint8_t* pre_key_b = (uint8_t*)&pre_key;

        index += (pre_key_b[index & 0x80000003] & 3) + 1;

        while (index < size && index + 3 >= size) {
            index--;
            end = true;
        }
    }

    for (int i = 0; i < size; i++)
        data[i] ^= 0xFF;
}

void encrypt_report(uint8_t* data, uint32_t size) {
    struct report_wrapper {
        uint64_t m_encrypted_report; // random key is appended
        uint32_t m_report_size;
    };

    // allocations are equiv to an ExAllocatePoolWithTag with tag 'BE', when ran in kernel.
    report_wrapper* wrapper = (report_wrapper*)malloc(24);
    if (wrapper) {
        wrapper->m_report_size = size + 4;

        wrapper->m_encrypted_report = (uint64_t)malloc(wrapper->m_report_size);
        if (wrapper->m_encrypted_report) {
            uint32_t key = 0x1234; // random key
            *(uint32_t*)(wrapper->m_encrypted_report) = key;

            int64_t report_start = (int64_t)wrapper->m_encrypted_report + 4;
            if (wrapper->m_encrypted_report == -4)
                report_start = (int64_t)data;

            uint8_t* buffer = &data[-report_start];
            uint32_t index = 0;
            uint8_t pre_key = 0;

            do {
                pre_key = index++ ^ key ^ buffer[report_start];
                pre_key ^= 0xA5;
                *(uint8_t*)(report_start++) = pre_key;
                key = ~pre_key;
            } while (index < size);

            // encrypted packet
            for (uint32_t i = 0; i < wrapper->m_report_size; i++)
                printf("%02x ", *(uint8_t*)(wrapper->m_encrypted_report + i));
            printf("\n");

            free((void*)wrapper->m_encrypted_report);
        }

        free(wrapper);
    }
}

void try_print_decryption(uint8_t* buffer, uint32_t size) {
    // pass in the raw encrypted buffer straight from the pipe. you can log them with pipe monitor.

    if (buffer[0] == 4) {
        printf("==[signatures]==\n");

        for (uint16_t i = 3; i < *(uint16_t*)(&buffer[1]); i++) {
            buffer[i] ^= 0x66;
            printf("%02x ", buffer[i]);
        } printf("\n");

        printf("\n");
        return;
    }

    if (buffer[0] == 6) {
        printf("==[hardware information]==\n");

        struct hardware_packet {
            char _0x0000[0xF5];
            char m_processor[0x30];
            
            const char* get_serial() {
                uint8_t serial_length = *(uint8_t*)((uint64_t)this + 0x125);
                char* buffer = new char[serial_length + 1];
                
                memset(buffer, 0, serial_length + 1);
                memcpy(buffer, (void*)((uint64_t)this + 0x126), serial_length);
                return buffer;
            }

            const char* get_smart_serial() {
                uint8_t serial_length = *(uint8_t*)((uint64_t)this + 0x125);
                uint8_t smart_serial_length = *(uint8_t*)((uint64_t)this + 0x126 + serial_length);
                char* buffer = new char[smart_serial_length + 1];

                memset(buffer, 0, smart_serial_length + 1);
                memcpy(buffer, (void*)((uint64_t)this + 0x126 + serial_length + 1), smart_serial_length);
                return buffer;
            }
        };

        hardware_packet* packet = (hardware_packet*)&buffer[5];

        crypt_second_stage(buffer, size);
        decrypt_hardware_information_generic(&buffer[5]);

        const char* serial = packet->get_serial();
        const char* smart_serial = packet->get_smart_serial();

        printf("[processor]\n%s\n", packet->m_processor);
        printf("[serial]\n%s\n", serial);
        printf("[smart serial]\n%s\n", smart_serial);

        printf("[buffer]\n");
        for (uint16_t i = 5; i < size; i++) {
            printf("%02x ", buffer[i]);
        } printf("\n");

        delete serial;
        delete smart_serial;

        printf("\n");
        return;
    }

    if (buffer[0] == 5)
        printf("==[response]==\n");
    else
        printf("==[request %i]==\n", buffer[0]);

    uint32_t index = 0;
    buffer = &buffer[1];

    while (index < size - 1) {
        uint16_t iteration_size = *(uint16_t*)buffer;
        buffer = &buffer[2];

        decrypt_generic(buffer, iteration_size);

        // examples
        switch (buffer[4]) {
            case 0:
                printf("[driver]\n");
                for (uint8_t i = 0; i < buffer[5]; i++)
                    printf("%c", buffer[i + 5 + 1]);
                printf("\n");
                break;

            default:
                for (uint16_t i = 4; i < iteration_size; i++)
                    printf("%02x ", buffer[i]);
                printf("\n");
                break;
        }

        buffer = &buffer[iteration_size];
        index += iteration_size + 2;
    }

    printf("\n");
}

namespace buffers {
    // example buffers logged with IO Ninja's Pipe Monitor
    static unsigned char driver_names[] = {
		0x05, 0x5D, 0x00, 0xCB, 0x05, 0xA1, 0x22, 0x6E, 0x1A, 0x01, 0x62, 0x60,
		0x68, 0x5D, 0x6E, 0x58, 0x64, 0x43, 0x61, 0x6B, 0x6F, 0x42, 0x64, 0x5A,
		0x74, 0x51, 0x2B, 0x57, 0x44, 0x6C, 0x53, 0x78, 0x4D, 0x68, 0x5B, 0x6E,
		0x75, 0x7E, 0x58, 0x57, 0x40, 0x4D, 0x47, 0x66, 0x4F, 0x5A, 0x60, 0x57,
		0x49, 0x7B, 0x7F, 0x7A, 0x23, 0x24, 0x28, 0x31, 0x5A, 0xF2, 0x9B, 0xF5,
		0xC2, 0x2E, 0x43, 0x21, 0xC6, 0xB7, 0xD6, 0xB0, 0x9D, 0x9A, 0x7D, 0x3E,
		0x7D, 0x7B, 0x62, 0x7C, 0x63, 0x7F, 0x2D, 0x5C, 0x3A, 0x46, 0x22, 0x47,
		0x70, 0x32, 0x75, 0x53, 0x78, 0x3C, 0x79, 0x34, 0x3B, 0x37, 0x3A, 0x38,
		0x4D, 0x00, 0xAB, 0x34, 0x4C, 0x08, 0x0E, 0x67, 0x63, 0x69, 0x4E, 0x62,
		0x4A, 0x72, 0x4D, 0x4C, 0x73, 0x4D, 0x6F, 0x64, 0x63, 0x4F, 0x76, 0x49,
		0x64, 0x40, 0x3D, 0x40, 0x50, 0x59, 0x69, 0x43, 0x75, 0x51, 0x65, 0x51,
		0x49, 0x68, 0x67, 0x71, 0x79, 0x5F, 0x45, 0x4F, 0x5E, 0x53, 0x52, 0x55,
		0x4A, 0x49, 0x4F, 0x57, 0x51, 0x50, 0x14, 0x0C, 0x1D, 0x07, 0x69, 0xE6,
		0x8A, 0xE7, 0x85, 0xE6, 0x86, 0xE7, 0xA1, 0x66, 0x02, 0x67, 0x0F, 0x97,
		0x51, 0xED, 0xCB, 0xF5, 0xE9, 0xF4, 0xE6, 0x49, 0x00, 0x01, 0x4F, 0x51,
		0x58, 0xA4, 0xD1, 0xD5, 0xDF, 0xF8, 0xD4, 0xFC, 0xC4, 0xFB, 0xFA, 0xC5,
		0xFB, 0xD9, 0xD2, 0xD5, 0xF9, 0xC0, 0xFF, 0xD2, 0xF6, 0x8B, 0xF6, 0xE6,
		0xCF, 0xFF, 0xD5, 0xE3, 0xC7, 0xF3, 0xC7, 0xDF, 0xFE, 0xF1, 0xE7, 0xEF,
		0xC9, 0xC4, 0xCF, 0xDC, 0xD3, 0xCF, 0xCA, 0xD7, 0xC3, 0x9B, 0x9F, 0x92,
		0x94, 0xFE, 0x45, 0x2F, 0x46, 0x28, 0x47, 0x2B, 0x46, 0x34, 0xC7, 0xA5,
		0xC4, 0x18, 0x84, 0x6F, 0x86, 0xCC, 0xF2, 0xEA, 0xF3, 0xED, 0x48, 0x00,
		0x8A, 0x71, 0xBF, 0x3F, 0x2F, 0x59, 0x5D, 0x57, 0x70, 0x5C, 0x74, 0x4C,
		0x73, 0x72, 0x4D, 0x73, 0x51, 0x5A, 0x5D, 0x71, 0x48, 0x77, 0x5A, 0x7E,
		0x03, 0x7E, 0x6E, 0x67, 0x57, 0x7D, 0x4B, 0x6F, 0x5B, 0x6F, 0x77, 0x56,
		0x59, 0x4F, 0x47, 0x61, 0x7B, 0x71, 0x60, 0x6D, 0x79, 0x7C, 0x69, 0x36,
		0x33, 0x3D, 0x3A, 0x4F, 0xF5, 0x9F, 0xF7, 0x9E, 0xF0, 0x9F, 0xF3, 0x8E,
		0x7C, 0x1E, 0x7E, 0x68, 0x96, 0x94, 0xF2, 0x77, 0x44, 0x5F, 0x47, 0x5E,
		0x98, 0x00, 0x0A, 0x51, 0x9A, 0x52, 0xAF, 0xB2, 0xA9, 0xCA, 0xC8, 0xC7,
		0xE9, 0xDB, 0xEE, 0xCF, 0xFE, 0xC2, 0xB4, 0xA5, 0x98, 0xA1, 0x8E, 0xB6,
		0xA2, 0xA8, 0x89, 0xB4, 0x8B, 0xA7, 0x8C, 0xBD, 0xA1, 0xA3, 0xAA, 0xBF,
		0xA8, 0xAC, 0x9F, 0xB6, 0xEE, 0xFE, 0xC3, 0xE9, 0xD0, 0x8D, 0xCB, 0x98,
		0xBB, 0xA5, 0xB5, 0xB6, 0xB5, 0xA1, 0xB9, 0xB7, 0x83, 0xA9, 0xA8, 0xB5,
		0xAA, 0xA6, 0xAD, 0xBC, 0x90, 0xBD, 0x9A, 0x9E, 0x99, 0x99, 0xF0, 0x98,
		0xB6, 0x9B, 0xAB, 0xC7, 0xA2, 0xCC, 0xDE, 0x5D, 0x4D, 0x5C, 0xB2, 0xF1,
		0xE5, 0xF0, 0xFA, 0x91, 0x99, 0x90, 0x58, 0x65, 0x2F, 0x7C, 0x86, 0xA1,
		0xA1, 0xA0, 0xA6, 0xA1, 0xE8, 0x84, 0xDD, 0x94, 0xC3, 0x89, 0xD8, 0x81,
		0xC9, 0xD4, 0xB1, 0xEB, 0xB5, 0xE0, 0xB9, 0xF9, 0xBE, 0xAB, 0xC9, 0x83,
		0xD9, 0x94, 0xCD, 0x83, 0xDD, 0x95, 0x97, 0xF7, 0xB8, 0xF4, 0xA2, 0xE4,
		0xB4, 0xF8, 0x40, 0xF2, 0x46, 0xF6, 0x5C, 0xFA, 0x06, 0x8B, 0x2C, 0x9D,
		0x21, 0x99, 0x3C, 0x83, 0x32, 0x95, 0x5F, 0x94, 0x5C, 0x95, 0x56, 0x00,
		0xDB, 0x66, 0x22, 0x6B, 0x7E, 0x0F, 0x14, 0x77, 0x75, 0x7D, 0x48, 0x7B,
		0x4D, 0x71, 0x56, 0x74, 0x7E, 0x7A, 0x57, 0x71, 0x6C, 0x68, 0x77, 0x08,
		0x72, 0x61, 0x49, 0x76, 0x5D, 0x68, 0x4D, 0x7E, 0x4B, 0x50, 0x62, 0x54,
		0x5A, 0x4E, 0x44, 0x0F, 0x5C, 0x5B, 0x11, 0x58, 0x04, 0x04, 0x0D, 0x0F,
		0x79, 0xCE, 0xBA, 0xCF, 0xE5, 0x5B, 0x33, 0x5A, 0xD8, 0x24, 0x48, 0x25,
		0x98, 0x88, 0xC9, 0xF7, 0xD1, 0xDD, 0xB9, 0xDC, 0xC6, 0xDD, 0x93, 0xC7,
		0xAE, 0xD0, 0xBE, 0xC6, 0xF8, 0xCB, 0x92, 0xED, 0x98, 0xA1, 0xB5, 0xA0,
		0xAA, 0xA1, 0x79, 0x00, 0x40, 0x71, 0x02, 0x5E, 0xE5, 0x99, 0x82, 0xE1,
		0xE3, 0xEB, 0xDE, 0xED, 0xDB, 0xE7, 0xC0, 0xE2, 0xE8, 0xEB, 0xDA, 0xE2,
		0xD8, 0xCF, 0xE4, 0xDD, 0xE6, 0xD3, 0xAE, 0xD6, 0xA6, 0xB9, 0x9A, 0xAB,
		0x98, 0xA5, 0xD0, 0xA0, 0xE8, 0xCC, 0xCC, 0x83, 0xC9, 0x98, 0x97, 0x93,
		0x92, 0xE1, 0x21, 0x50, 0x26, 0x51, 0xA9, 0xDC, 0xB6, 0xB9, 0x41, 0x28,
		0x46, 0x20, 0x87, 0x27, 0x25, 0x46, 0x02, 0x63, 0x05, 0x62, 0x06, 0x2E,
		0x5D, 0x25, 0x4F, 0x39, 0x54, 0x24, 0x5E, 0x37, 0x05, 0x41, 0x38, 0x47,
		0x35, 0x4D, 0x2E, 0x48, 0x62, 0x21, 0x48, 0x33, 0x59, 0x21, 0x4C, 0x33,
		0x54, 0x77, 0x34, 0x5A, 0x31, 0x46, 0x23, 0x52, 0x01, 0x58, 0x09, 0x5C,
		0x0B, 0x40, 0x05, 0x18, 0x7A, 0x3C, 0x6E, 0x33, 0x6C, 0x28, 0x74, 0x24,
		0x7C, 0x57, 0x7F, 0x56, 0x78, 0x78, 0x00, 0xF2, 0x63, 0xB2, 0x3A, 0x57,
		0x2A, 0x31, 0x52, 0x50, 0x58, 0x6D, 0x5E, 0x68, 0x54, 0x73, 0x51, 0x5B,
		0x5F, 0x72, 0x54, 0x6A, 0x44, 0x61, 0x1B, 0x67, 0x74, 0x5C, 0x63, 0x48,
		0x7D, 0x58, 0x6B, 0x5E, 0x45, 0x6C, 0x4B, 0x50, 0x4E, 0x00, 0x4D, 0x1D,
		0x11, 0x14, 0x1A, 0x68, 0x8B, 0xF9, 0x88, 0x4E, 0x48, 0x3E, 0x4B, 0x21,
		0x2A, 0x40, 0x29, 0x79, 0x02, 0xA9, 0xA4, 0x76, 0x34, 0x54, 0x35, 0x53,
		0x34, 0x1D, 0x11, 0x68, 0x01, 0x76, 0x1C, 0x6D, 0x14, 0x7C, 0x41, 0x04,
		0x7E, 0x00, 0x75, 0x0C, 0x6C, 0x0B, 0x3E, 0x7C, 0x16, 0x6C, 0x01, 0x78,
		0x16, 0x68, 0x00, 0x22, 0x62, 0x0D, 0x61, 0x17, 0x71, 0x01, 0x6D, 0x35,
		0x67, 0x33, 0x63, 0x29, 0x6F, 0x73, 0x1E, 0x59, 0x08, 0x54, 0x0C, 0x49,
		0x16, 0x47, 0x00, 0x2A, 0x01, 0x29, 0x00, 0x5D, 0x00, 0x27, 0x66, 0x0D,
		0x60, 0x82, 0xF1, 0xEA, 0x89, 0x8B, 0x83, 0xB6, 0x85, 0xB3, 0x8F, 0xA8,
		0x8A, 0x80, 0x84, 0xA9, 0x8F, 0xB1, 0x9F, 0xBA, 0xC0, 0xBC, 0xAF, 0x87,
		0xB8, 0x93, 0xA6, 0x83, 0xB0, 0x85, 0x9E, 0xAE, 0x8F, 0x90, 0x9D, 0x88,
		0x9E, 0x8E, 0x92, 0xC0, 0xCE, 0xC5, 0xC5, 0xB5, 0x24, 0x52, 0x25, 0xC1,
		0x7F, 0x15, 0x7E, 0xA6, 0x7D, 0x13, 0x7C, 0x1E, 0x11, 0xE5, 0xD9, 0x29,
		0x0F, 0x69, 0x0E, 0x6A, 0x0F, 0x41, 0x33, 0x49, 0x32, 0x43, 0x7C, 0x34,
		0x4C, 0x3D, 0x46, 0x38, 0x46, 0x3C, 0x44, 0x37, 0x5B, 0x71, 0x33, 0x55,
		0x3F, 0x1F, 0x10, 0x1C, 0x11, 0x13, 0x5C, 0x00, 0x58, 0x48, 0x01, 0x0F,
		0xFD, 0x85, 0x9E, 0xFD, 0xFF, 0xF7, 0xC2, 0xF1, 0xC7, 0xFB, 0xDC, 0xFE,
		0xF4, 0xF0, 0xDD, 0xFB, 0xC5, 0xEB, 0xCE, 0xB4, 0xC8, 0xDB, 0xF3, 0xCC,
		0xE7, 0xD2, 0xF7, 0xC4, 0xF1, 0xEA, 0xC0, 0xF5, 0xE9, 0xBC, 0xB7, 0xB7,
		0xBA, 0xC5, 0x79, 0x04, 0x76, 0xFD, 0x03, 0x72, 0x04, 0x7B, 0x9F, 0xEA,
		0x80, 0x07, 0x6B, 0x32, 0x0D, 0x9A, 0xE8, 0x85, 0xE7, 0x84, 0xE4, 0xD7,
		0xD8, 0xC9, 0xC8, 0xDF, 0xA7, 0xD9, 0xA5, 0x9C, 0xD6, 0xAC, 0xD3, 0xA6,
		0xDA, 0xA6, 0xDA, 0xA4, 0xD5, 0xBB, 0x83, 0xB6, 0xF5, 0x90, 0xFB, 0xDC,
		0xD2, 0xDD, 0xD1, 0xDC
	};

	static unsigned char signatures_1[] = {
		0x04, 0xDF, 0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0x66, 0x50, 0x66, 0x66,
		0x09, 0x13, 0x14, 0x46, 0x15, 0x03, 0x08, 0x15, 0x46, 0x0F, 0x08, 0x46,
		0x15, 0x05, 0x09, 0x16, 0x03, 0x46, 0x0F, 0x15, 0x46, 0x51, 0x55, 0x4D,
		0x66, 0x66, 0x66, 0x66, 0x33, 0x28, 0x36, 0x27, 0xE5, 0x04, 0x01, 0x0A,
		0x00, 0x00, 0x00, 0x17, 0x6D, 0x74, 0x66, 0x23, 0x0A, 0x07, 0x41, 0x15,
		0x46, 0xE5, 0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0x0E, 0x05, 0x66, 0x66,
		0x2D, 0x66, 0x14, 0x66, 0x1F, 0x66, 0x16, 0x66, 0x12, 0x66, 0x09, 0x66,
		0x08, 0x66, 0x46, 0x66, 0x27, 0x66, 0x21, 0x66, 0x66, 0x66, 0x66, 0x66,
		0x24, 0x66, 0x6B, 0x66, 0x67, 0x66, 0x20, 0x66, 0xE5, 0x04, 0x01, 0x24,
		0x00, 0x00, 0x00, 0x32, 0x24, 0x66, 0x66, 0x34, 0x07, 0x0F, 0x08, 0x04,
		0x09, 0x11, 0x46, 0x35, 0x0F, 0x1E, 0x66, 0x46, 0x66, 0x66, 0x66, 0xA2,
		0x23, 0x57, 0x67, 0x66, 0x46, 0x57, 0x67, 0x82, 0x23, 0x57, 0x67, 0x11,
		0x4B, 0x57, 0x67, 0xEE, 0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0x12, 0x5B,
		0x6B, 0x66, 0x2F, 0x2E, 0x22, 0x34, 0x66, 0x66, 0x67, 0x66, 0x66, 0x66,
		0x66, 0x9A, 0x6E, 0x60, 0x66, 0x66, 0x66, 0xFE, 0x33, 0x1C, 0xD6, 0x66,
		0x66, 0x46, 0x66, 0x2F, 0x22, 0x27, 0x32, 0x1E, 0xFA, 0x8A, 0xC7, 0x05,
		0x01, 0x24, 0x00, 0x00, 0x00, 0x06, 0xD6, 0xF0, 0x66, 0x20, 0x52, 0x66,
		0x66, 0x20, 0x53, 0x66, 0x66, 0x20, 0x50, 0x66, 0x66, 0x20, 0x51, 0x66,
		0x66, 0x20, 0x5E, 0x66, 0x66, 0x20, 0x5F, 0x66, 0x66, 0x20, 0x57, 0x56,
		0x66, 0x20, 0x57, 0x57, 0x66, 0xC9, 0x05, 0x01, 0x24, 0x00, 0x00, 0x00,
		0x96, 0x55, 0x66, 0x66, 0x46, 0x23, 0x35, 0x36, 0x6C, 0x66, 0x66, 0x66,
		0x44, 0x63, 0xF5, 0x7F, 0x67, 0x66, 0x66, 0x66, 0x26, 0x5E, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x67, 0x66, 0x66, 0x66,
		0xCC, 0x05, 0x01, 0x24, 0x00, 0x00, 0x00, 0x86, 0x1C, 0x66, 0x66, 0x15,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x44, 0x63, 0xF5, 0x7F, 0x64,
		0x66, 0x66, 0x66, 0x5A, 0xE1, 0x66, 0x66, 0x67, 0x66, 0x66, 0x66, 0x22,
		0xEE, 0x66, 0x66, 0x64, 0x66, 0x66, 0x66, 0xCC, 0x05, 0x01, 0x24, 0x00,
		0x00, 0x00, 0x06, 0xD3, 0x66, 0x66, 0x45, 0x46, 0x20, 0x55, 0x46, 0x4B,
		0x58, 0x46, 0x23, 0x35, 0x36, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46,
		0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46,
		0x46, 0x46, 0xCC, 0x05, 0x01, 0x24, 0x00, 0x00, 0x00, 0x46, 0xA6, 0x64,
		0x66, 0x15, 0x5C, 0x46, 0x43, 0x0A, 0x0A, 0x13, 0x6C, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x46, 0x46, 0x46, 0x46, 0x43, 0x0A, 0x0A,
		0x1E, 0x6C, 0x66, 0x66, 0x66, 0x25, 0x5C, 0x3A, 0x66, 0xCC, 0x05, 0x01,
		0x24, 0x00, 0x00, 0x00, 0x36, 0x0F, 0x65, 0x66, 0x28, 0x12, 0x21, 0x02,
		0x0F, 0x22, 0x02, 0x22, 0x22, 0x2F, 0x28, 0x03, 0x12, 0x22, 0x0F, 0x15,
		0x16, 0x37, 0x13, 0x03, 0x14, 0x1F, 0x2B, 0x0F, 0x14, 0x07, 0x05, 0x07,
		0x15, 0x12, 0x22, 0x0F, 0x06, 0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0x5A,
		0x66, 0x68, 0x66, 0x05, 0x66, 0x14, 0x66, 0x09, 0x66, 0x66, 0x66, 0x22,
		0x66, 0x68, 0x66, 0x67, 0x66, 0x20, 0x66, 0x0F, 0x66, 0x0A, 0x66, 0x03,
		0x66, 0x22, 0x66, 0x03, 0x66, 0x15, 0x66, 0x05, 0x66, 0x14, 0x66, 0x06,
		0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0x0A, 0x91, 0x6B, 0x66, 0x66, 0x66,
		0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x22, 0x66, 0x68, 0x66, 0x67, 0x66,
		0x20, 0x66, 0x0F, 0x66, 0x0A, 0x66, 0x03, 0x66, 0x22, 0x66, 0x03, 0x66,
		0x15, 0x66, 0x05, 0x66, 0x14, 0x66, 0x07, 0x04, 0x01, 0x24, 0x00, 0x00,
		0x00, 0x0F, 0x5F, 0x72, 0x66, 0x39, 0x07, 0x0D, 0x2D, 0x03, 0x1F, 0x66,
		0x10, 0x07, 0x0A, 0x13, 0x03, 0x66, 0x01, 0x03, 0x12, 0x39, 0x0A, 0x14,
		0x2D, 0x03, 0x1F, 0x66, 0x15, 0x03, 0x12, 0x39, 0x0A, 0x14, 0x2D, 0x03,
		0x1F, 0x11, 0x04, 0x01, 0x24, 0x00, 0x00, 0x00, 0xA6, 0x07, 0x66, 0x66,
		0x05, 0x66, 0x14, 0x66, 0x0F, 0x66, 0x16, 0x66, 0x12, 0x66, 0x0F, 0x66,
		0x09, 0x66, 0x08, 0x66, 0x66, 0x66, 0x66, 0x66, 0x34, 0x66, 0x07, 0x66,
		0x01, 0x66, 0x03, 0x66, 0x2F, 0x66, 0x08, 0x66, 0x43, 0x05, 0x01, 0x24,
		0x00, 0x00, 0x00, 0x76, 0x69, 0x6B, 0x66, 0x04, 0x1F, 0x46, 0x20, 0x03,
		0x03, 0x02, 0x03, 0x14, 0x1A, 0x27, 0x08, 0x12, 0x0F, 0x46, 0x27, 0x20,
		0x2D, 0x1A, 0x2C, 0x09, 0x0F, 0x08, 0x1A, 0x46, 0x46, 0x2B, 0x03, 0x07,
		0x12, 0x46, 0x24, 0xE2, 0x05, 0x06, 0x0B, 0x00, 0x00, 0x00, 0x3A, 0x08,
		0x0F, 0x0B, 0x02, 0x27, 0x48, 0x02, 0x0A, 0x0A, 0x66, 0x7B, 0x05, 0x06,
		0x0C, 0x00, 0x00, 0x00, 0x3D, 0x66, 0x43, 0x66, 0x02, 0x66, 0x2B, 0x66,
		0x3B, 0x66, 0x66, 0x66, 0xF3, 0x04, 0x07, 0x0D, 0x00, 0x00, 0x00, 0x6A,
		0x34, 0x50, 0x35, 0x4B, 0x33, 0x0A, 0x12, 0x0F, 0x0B, 0x07, 0x12, 0x03,
		0xF3, 0x04, 0x07, 0x21, 0x00, 0x00, 0x00, 0x68, 0x27, 0x13, 0x12, 0x09,
		0x2E, 0x09, 0x12, 0x0D, 0x03, 0x1F, 0x48, 0x03, 0x1E, 0x03, 0x08, 0x66,
		0x09, 0x66, 0x14, 0x66, 0x03, 0x66, 0x05, 0x66, 0x09, 0x66, 0x0F, 0x66,
		0x0A, 0x66, 0x66, 0x66, 0xE8, 0x04, 0x07, 0x36, 0x00, 0x00, 0x00, 0x68,
		0x27, 0x13, 0x12, 0x09, 0x2E, 0x09, 0x12, 0x0D, 0x03, 0x1F, 0x48, 0x03,
		0x1E, 0x03, 0x0B, 0x09, 0x13, 0x15, 0x03, 0x3E, 0x3F, 0x4E, 0x4B, 0x54,
		0x4A, 0x54, 0x4F, 0x46, 0x6B, 0x6C, 0x15, 0x0A, 0x03, 0x03, 0x16, 0x46,
		0x4B, 0x57, 0x46, 0x6B, 0x6C, 0x0B, 0x09, 0x13, 0x15, 0x03, 0x3E, 0x3F,
		0x4E, 0x57, 0x4A, 0x57, 0x4F, 0xE2, 0x05, 0x07, 0x0F, 0x00, 0x00, 0x00,
		0x61, 0x12, 0x09, 0x14, 0x48, 0x03, 0x1E, 0x03, 0x27, 0x0F, 0x0B, 0x04,
		0x09, 0x12, 0x66, 0xF6, 0x05, 0x07, 0x2C, 0x00, 0x00, 0x00, 0x6D, 0x05,
		0x14, 0x03, 0x02, 0x11, 0x0F, 0x1C, 0x48, 0x03, 0x1E, 0x03, 0x22, 0x66,
		0x13, 0x66, 0x0B, 0x66, 0x16, 0x66, 0x0F, 0x66, 0x08, 0x66, 0x01, 0x66,
		0x46, 0x66, 0x00, 0x66, 0x0F, 0x66, 0x14, 0x66, 0x15, 0x66, 0x12, 0x66,
		0x46, 0x66, 0x43, 0x66, 0x02, 0x66, 0xDE, 0x05, 0x07, 0x2C, 0x00, 0x00,
		0x00, 0x6D, 0x05, 0x0E, 0x07, 0x14, 0x0B, 0x07, 0x16, 0x48, 0x03, 0x1E,
		0x03, 0x62, 0xA8, 0x62, 0x99, 0x62, 0x95, 0x62, 0xBD, 0x6E, 0x99, 0x62,
		0xC1, 0x7E, 0x66, 0x62, 0x75, 0x62, 0xA8, 0x62, 0x99, 0x62, 0x80, 0x62,
		0x44, 0x4A, 0x66, 0x62, 0x75, 0x62, 0xA8, 0x62, 0x99, 0xE4, 0x05, 0x07,
		0x2C, 0x00, 0x00, 0x00, 0x6D, 0x0B, 0x15, 0x16, 0x07, 0x0F, 0x08, 0x12,
		0x48, 0x03, 0x1E, 0x03, 0x2A, 0x07, 0x08, 0x02, 0x0F, 0x08, 0x01, 0x46,
		0x35, 0x13, 0x05, 0x05, 0x03, 0x15, 0x15, 0x00, 0x13, 0x0A, 0x4A, 0x46,
		0x36, 0x0A, 0x03, 0x07, 0x15, 0x03, 0x46, 0x2A, 0x07, 0x08, 0x02, 0x0F,
		0x0D, 0x04, 0x07, 0x2C, 0x00, 0x00, 0x00, 0x6C, 0x05, 0x0E, 0x14, 0x09,
		0x0B, 0x03, 0x48, 0x03, 0x1E, 0x03, 0x0E, 0x12, 0x12, 0x16, 0x5C, 0x49,
		0x49, 0x01, 0x0E, 0x09, 0x15, 0x12, 0x11, 0x07, 0x14, 0x03, 0x05, 0x0E,
		0x03, 0x07, 0x12, 0x15, 0x48, 0x08, 0x03, 0x12, 0x49, 0x59, 0x16, 0x09,
		0x14, 0x12, 0x5B, 0x0D, 0x04, 0x07, 0x2C, 0x00, 0x00, 0x00, 0x6C, 0x0B,
		0x15, 0x03, 0x02, 0x01, 0x03, 0x48, 0x03, 0x1E, 0x03, 0x0E, 0x12, 0x12,
		0x16, 0x5C, 0x49, 0x49, 0x01, 0x0E, 0x09, 0x15, 0x12, 0x11, 0x07, 0x14,
		0x03, 0x05, 0x0E, 0x03, 0x07, 0x12, 0x15, 0x48, 0x08, 0x03, 0x12, 0x49,
		0x59, 0x16, 0x09, 0x14, 0x12, 0x5B, 0x0D, 0x04, 0x07, 0x2D, 0x00, 0x00,
		0x00, 0x6D, 0x00, 0x0F, 0x14, 0x03, 0x00, 0x09, 0x1E, 0x48, 0x03, 0x1E,
		0x03, 0x0E, 0x12, 0x12, 0x16, 0x5C, 0x49, 0x49, 0x01, 0x0E, 0x09, 0x15,
		0x12, 0x11, 0x07, 0x14, 0x03, 0x05, 0x0E, 0x03, 0x07, 0x12, 0x15, 0x48,
		0x08, 0x03, 0x12, 0x49, 0x59, 0x16, 0x09, 0x14, 0x12, 0x5B, 0xE5, 0x05,
		0x07, 0x29, 0x00, 0x00, 0x00, 0x6A, 0x03, 0x1E, 0x16, 0x0A, 0x09, 0x14,
		0x03, 0x14, 0x48, 0x03, 0x1E, 0x03, 0x46, 0x66, 0x20, 0x66, 0x36, 0x66,
		0x35, 0x66, 0x46, 0x66, 0x4E, 0x66, 0x56, 0x66, 0x46, 0x66, 0x0B, 0x66,
		0x15, 0x66, 0x4F, 0x66, 0x46, 0x66, 0x1A, 0x66, 0x46, 0x66, 0xE2, 0x05,
		0x07, 0x28, 0x00
	};

	static unsigned char signatures_2[] = {
		0x04, 0x61, 0x02, 0x11, 0x0B, 0x48, 0x03, 0x1E, 0x03, 0x4A, 0x11, 0x0F,
		0x2A, 0x40, 0x15, 0x25, 0x13, 0x42, 0x5D, 0x58, 0x17, 0x30, 0x48, 0x3B,
		0x0A, 0x3D, 0x0E, 0x4E, 0x3D, 0x03, 0x3E, 0x26, 0x42, 0x55, 0x41, 0x40,
		0x4D, 0x48, 0x32, 0x0C, 0x52, 0xA5, 0x05, 0x07, 0x17, 0x00, 0x00, 0x00,
		0x77, 0x21, 0x07, 0x0B, 0x03, 0x29, 0x10, 0x03, 0x14, 0x0A, 0x07, 0x1F,
		0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03, 0x99, 0x76, 0xE2, 0xA6, 0x8F, 0xA5,
		0x05, 0x07, 0x27, 0x00, 0x00, 0x00, 0x77, 0x21, 0x07, 0x0B, 0x03, 0x29,
		0x10, 0x03, 0x14, 0x0A, 0x07, 0x1F, 0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03,
		0x69, 0x27, 0x66, 0x0A, 0x66, 0x12, 0x66, 0x4D, 0x66, 0x54, 0x66, 0x46,
		0x66, 0x00, 0x66, 0x0F, 0x66, 0x14, 0x66, 0x03, 0x66, 0xA5, 0x05, 0x07,
		0x1E, 0x00, 0x00, 0x00, 0x77, 0x21, 0x07, 0x0B, 0x03, 0x29, 0x10, 0x03,
		0x14, 0x0A, 0x07, 0x1F, 0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03, 0x57, 0x48,
		0x02, 0x0A, 0x0A, 0x66, 0x07, 0x05, 0x05, 0x05, 0x05, 0x66, 0xA5, 0x05,
		0x07, 0x32, 0x00, 0x00, 0x00, 0x77, 0x21, 0x07, 0x0B, 0x03, 0x29, 0x10,
		0x03, 0x14, 0x0A, 0x07, 0x1F, 0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03, 0x2C,
		0x66, 0x13, 0x66, 0x14, 0x66, 0x07, 0x66, 0x15, 0x66, 0x15, 0x66, 0x0F,
		0x66, 0x05, 0x66, 0x46, 0x66, 0x36, 0x66, 0x03, 0x66, 0x14, 0x66, 0x0F,
		0x66, 0x09, 0x66, 0x02, 0x66, 0x66, 0x66, 0xA5, 0x05, 0x07, 0x28, 0x00,
		0x00, 0x00, 0x77, 0x21, 0x07, 0x0B, 0x03, 0x29, 0x10, 0x03, 0x14, 0x0A,
		0x07, 0x1F, 0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03, 0x05, 0x66, 0x05, 0x66,
		0x05, 0x66, 0x16, 0x66, 0x07, 0x66, 0x08, 0x66, 0x48, 0x66, 0x05, 0x66,
		0x09, 0x66, 0x0B, 0x66, 0x66, 0x66, 0x8E, 0x05, 0x07, 0x12, 0x00, 0x00,
		0x00, 0x6F, 0x0A, 0x15, 0x07, 0x15, 0x15, 0x48, 0x03, 0x1E, 0x03, 0x66,
		0x66, 0x66, 0x69, 0x63, 0x8D, 0x61, 0xDE, 0x7D, 0x05, 0x07, 0x19, 0x00,
		0x00, 0x00, 0x77, 0x21, 0x07, 0x0B, 0x03, 0x29, 0x10, 0x03, 0x14, 0x0A,
		0x07, 0x1F, 0x33, 0x2F, 0x48, 0x03, 0x1E, 0x03, 0x2E, 0x09, 0x0B, 0x03,
		0xDA, 0x9A, 0xA9, 0x85, 0x05, 0x07, 0x1B, 0x00, 0x00, 0x00, 0x6F, 0x0A,
		0x15, 0x07, 0x15, 0x15, 0x48, 0x03, 0x1E, 0x03, 0x3A, 0x3A, 0x48, 0x3A,
		0x16, 0x0F, 0x16, 0x03, 0x3A, 0x27, 0x24, 0x36, 0x0F, 0x16, 0x03, 0x57,
		0x66, 0x85, 0x05, 0x07, 0x1B, 0x00, 0x00, 0x00, 0x6F, 0x05, 0x15, 0x14,
		0x15, 0x15, 0x48, 0x03, 0x1E, 0x03, 0x3A, 0x3A, 0x48, 0x3A, 0x16, 0x0F,
		0x16, 0x03, 0x3A, 0x27, 0x24, 0x36, 0x0F, 0x16, 0x03, 0x57, 0x66, 0x36,
		0x05, 0x06, 0x20, 0x00, 0x00, 0x00, 0x26, 0x94, 0xCC, 0x73, 0x09, 0x6E,
		0xB4, 0xEF, 0x28, 0xFC, 0xD2, 0x2E, 0xF3, 0x53, 0xB5, 0x29, 0xFA, 0x36,
		0x29, 0x35, 0x2F, 0x32, 0x2F, 0x29, 0x28, 0x66, 0x66, 0x66, 0x66, 0x25,
		0x29, 0x2A, 0x8C, 0x05, 0x06, 0x20, 0x00, 0x00, 0x00, 0x03, 0x15, 0x15,
		0x2B, 0x03, 0x0B, 0x09, 0x14, 0x1F, 0x66, 0xE5, 0x63, 0x35, 0x0A, 0x03,
		0x03, 0x16, 0x66, 0x96, 0x66, 0x25, 0x14, 0x03, 0x07, 0x12, 0x03, 0x32,
		0x0E, 0x14, 0x03, 0x07, 0x02, 0x90, 0x05, 0x07, 0x2A, 0x00, 0x00, 0x00,
		0x6F, 0x0A, 0x15, 0x07, 0x15, 0x15, 0x48, 0x03, 0x1E, 0x03, 0x2E, 0xEB,
		0x6B, 0x2F, 0xC8, 0x67, 0x66, 0x8F, 0xCE, 0x5A, 0x66, 0x66, 0xAA, 0xAA,
		0xAA, 0xAA, 0xA4, 0x66, 0x66, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
		0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x9B, 0x05, 0x07, 0x2A, 0x00, 0x00,
		0x00, 0x6F, 0x0A, 0x15, 0x07, 0x15, 0x15, 0x48, 0x03, 0x1E, 0x03, 0x2E,
		0xE5, 0x8A, 0x4E, 0x27, 0xDE, 0x62, 0x66, 0x66, 0x66, 0x2E, 0xEB, 0x73,
		0x09, 0xC3, 0x66, 0x66, 0x2E, 0xEB, 0x6B, 0x4E, 0x96, 0x66, 0x66, 0x8E,
		0xA5, 0x3B, 0x66, 0x66, 0x2E, 0xEB, 0x6B, 0x9D, 0x05, 0x07, 0x2A, 0x00,
		0x00, 0x00, 0x6F, 0x0A, 0x15, 0x07, 0x15, 0x15, 0x48, 0x03, 0x1E, 0x03,
		0x55, 0xA6, 0x2E, 0xEF, 0x63, 0xE9, 0xE0, 0x66, 0x66, 0x2E, 0xEF, 0x63,
		0xF6, 0xE0, 0x66, 0x66, 0x2E, 0xEF, 0x63, 0xF7, 0xE0, 0x66, 0x66, 0x2E,
		0xEB, 0x63, 0x24, 0x35, 0x66, 0x66, 0x2E, 0xEF, 0x04, 0x00, 0x00, 0x00
	};

	static unsigned char hardware_information[] = {
		0x06, 0x2A, 0x75, 0xB5, 0x17, 0x04, 0xFF, 0x64, 0xB4, 0x70, 0x81, 0xB0,
		0x73, 0x99, 0x01, 0xF0, 0x9D, 0x93, 0x05, 0xF0, 0x99, 0x93, 0x7A, 0xD9,
		0xAC, 0xBA, 0x50, 0xBD, 0xCC, 0x0B, 0xD6, 0x22, 0x88, 0xE1, 0xB4, 0x44,
		0xEA, 0x52, 0x00, 0xD9, 0x9C, 0x6F, 0xBC, 0xA1, 0x42, 0x88, 0x3A, 0xEB,
		0xA6, 0x88, 0x0F, 0xDA, 0x93, 0xB9, 0x13, 0xDA, 0x8F, 0xB9, 0x73, 0xBE,
		0xEF, 0x08, 0xF5, 0x21, 0xAB, 0xE2, 0xC1, 0x11, 0x9F, 0x07, 0x35, 0x8C,
		0xA9, 0x3A, 0x89, 0xF4, 0x77, 0xDD, 0x0F, 0xBE, 0x93, 0xDD, 0x6F, 0xDA,
		0xF3, 0xB9, 0x73, 0xDA, 0xEF, 0xB9, 0x13, 0xBE, 0x8F, 0x08, 0x95, 0x21,
		0xCB, 0xE2, 0xF8, 0x0C, 0xE2, 0x1A, 0x08, 0x91, 0x94, 0x27, 0x9C, 0x93,
		0x30, 0xBA, 0x48, 0xD9, 0xD4, 0xBA, 0xF7, 0x54, 0x5D, 0x37, 0xDD, 0x54,
		0x41, 0x37, 0xAF, 0x60, 0x71, 0xD6, 0x6B, 0xFF, 0x35, 0x3C, 0x5D, 0x83,
		0x4D, 0x95, 0x67, 0x1E, 0xFB, 0xA8, 0x6B, 0x96, 0xD5, 0xBF, 0xAD, 0xDC,
		0x31, 0xBF, 0xDF, 0xE8, 0x01, 0x8B, 0x81, 0xE8, 0x1D, 0x8B, 0x9F, 0xA0,
		0x51, 0x16, 0x4B, 0x3F, 0x15, 0xFC, 0xF1, 0xB7, 0x99, 0xA1, 0x73, 0x2A,
		0xEF, 0x9C, 0xBC, 0x54, 0x43, 0x61, 0x53, 0x02, 0xCF, 0x61, 0x7F, 0xF6,
		0xF9, 0x8D, 0x83, 0xCE, 0x1B, 0xAD, 0x84, 0xC9, 0x18, 0x7F, 0x02, 0x56,
		0x5C, 0x95, 0x1C, 0x0B, 0x05, 0x1D, 0xAF, 0x96, 0x33, 0x20, 0x20, 0x90,
		0x93, 0xB9, 0xEB, 0xDA, 0x77, 0xB9, 0xC7, 0xE0, 0x47, 0x8E, 0xC4, 0xED,
		0x58, 0x8E, 0xCB, 0xD4, 0xBD, 0xC4, 0xD9, 0xED, 0x87, 0x2E, 0xDF, 0xEF,
		0x81, 0xF9, 0x6B, 0x72, 0xF7, 0xC4, 0xE5, 0x38, 0x1B, 0x11, 0x63, 0x72,
		0xFF, 0x11, 0x9F, 0x84, 0xB8, 0x52, 0x38, 0x31, 0xA4, 0x52, 0x7D, 0x3D,
		0xE8, 0xEF, 0x80, 0xED, 0xDD, 0x31, 0x8E, 0xBE, 0xC7, 0xBF, 0xF8, 0x39,
		0x6C, 0x8F, 0x16, 0x0B, 0xF0, 0x37, 0xE6, 0x78, 0x67, 0x0C, 0xBA, 0x1F,
		0x04, 0x61, 0x88, 0x04, 0x02, 0x67, 0x86, 0x19, 0x48, 0xAF, 0x52, 0x86,
		0x0C, 0x45, 0x54, 0x84, 0x0A, 0x92, 0xE0, 0x19, 0x7C, 0x8F, 0x61, 0x4E,
		0xE5, 0x60, 0x98, 0x70, 0x0B, 0x73, 0x87, 0x63, 0x61, 0x1E, 0x87, 0x18,
		0x17, 0x60, 0xDA, 0x50, 0x31, 0x80, 0x3A, 0xA8, 0x7A, 0x65, 0x41, 0xA1,
		0x66, 0xB6, 0xB5, 0x49, 0x54, 0xF7, 0x4F, 0x03, 0xD4, 0x3F, 0xBB, 0x37,
		0x6C, 0xEA, 0xA7, 0x7F, 0xCF, 0xD4, 0xC9, 0x00, 0xBF, 0x05, 0x66, 0xB4,
		0x59, 0xD4, 0xCC, 0x1A, 0x20, 0x2B, 0xCC, 0x47, 0x88, 0x88, 0xC0, 0x0E,
		0x9C, 0xE8, 0x85, 0x88, 0x47, 0x12, 0x40, 0x1A, 0x2A
	};
};

int main() {
    try_print_decryption(buffers::driver_names, sizeof(buffers::driver_names));
    try_print_decryption(buffers::signatures_1, sizeof(buffers::driver_names));
    try_print_decryption(buffers::signatures_2, sizeof(buffers::driver_names));
    try_print_decryption(buffers::hardware_information, sizeof(buffers::hardware_information));

	system("pause");
    return 0;
}