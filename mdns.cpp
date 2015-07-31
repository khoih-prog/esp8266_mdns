#include <Arduino.h>
#include "mdns.h"

namespace mdns {


// A UDP instance to let us send and receive packets over UDP.
WiFiUDP Udp;

// Helper function to display formatted data.
void PrintHex(unsigned char data) {
  char tmp[2];
  sprintf(tmp, "%02X", data);
  Serial.print(tmp);
  Serial.print(" ");
}

bool MDns::Check() {
  if (!init) {
    init = true;
    Serial.println("Initilising Multicast.");
    Udp.beginMulticast(WiFi.localIP(), IPAddress(224, 0, 0, 251), MDNS_TARGET_PORT);
  }
  data_size = Udp.parsePacket();
  if ( data_size > 12) {
    // We've received a packet which is long enough to contain useful data so
    // read the data from it.
    // TODO think about what to do if a packet is larger than buffer.
    Udp.read(data_buffer, data_size); // read the packet into the buffer


    // data_buffer[0] and data_buffer[1] contain the Query ID field which is unused in mDNS.

    // data_buffer[2] and data_buffer[3] are DNS flags which are mostly unused in mDNS.
    type = !(data_buffer[2] & 0b10000000);  // If it's not a query, it's an answer.
    truncated = data_buffer[2] & 0b00000010;  // If it's truncated we can expect more data soon so we should wait for additional recods before deciding whether to respond.
    if (data_buffer[3] & 0b00001111) {
      // Non zero Response code implies error.
      return false;
    }

    // Number of incoming queries.
    query_count = (data_buffer[4] << 8) + data_buffer[5];

    // Number of incoming answers.
    answer_count = (data_buffer[6] << 8) + data_buffer[7];

    // Number of incoming Name Server resource records.
    ns_count = (data_buffer[8] << 8) + data_buffer[9];

    // Number of incoming Additional resource records.
    ar_count = (data_buffer[10] << 8) + data_buffer[11];

#ifdef DEBUG_OUTPUT
    Display();
#endif  // DEBUG_OUTPUT

    // Start of Data section.
    buffer_pointer = 12;

    for (int i_question = 0; i_question < query_count; i_question++) {
      struct Query query = Parse_Query();
      if (query.valid) {
        if (p_query_function_) {
          // Since a callback function has been registered, execute it.
          p_query_function_(query);
        }
      }
#ifdef DEBUG_OUTPUT
      query.Display();
#endif  // DEBUG_OUTPUT
    }

    for (int i_answer = 0; i_answer < (answer_count + ns_count + ar_count); i_answer++) {
      struct Answer answer = Parse_Answer();
      if (answer.valid) {
        if (p_answer_function_) {
          // Since a callback function has been registered, execute it.
          p_answer_function_(answer);
        }
      }
#ifdef DEBUG_OUTPUT
      answer.Display();
#endif  // DEBUG_OUTPUT
    }

#ifdef DEBUG_RAW
    DisplayRawPacket();
#endif  // DEBUG_RAW

    return true;
  }
  return false;
}

void MDns::Clear() {
  data_buffer[0] = 0;     // Query ID field which is unused in mDNS.
  data_buffer[1] = 0;     // Query ID field which is unused in mDNS.
  data_buffer[2] = 0;     // 0b00000000 for Query, 0b10000000 for Answer.
  data_buffer[3] = 0;     // DNS flags which are mostly unused in mDNS.
  data_buffer[4] = 0;     // Number of queries.
  data_buffer[5] = 0;     // Number of queries.
  data_buffer[6] = 0;     // Number of answers.
  data_buffer[7] = 0;     // Number of answers.
  data_buffer[8] = 0;     // Number of Server esource records.
  data_buffer[9] = 0;     // Number of Server esource records.
  data_buffer[10] = 0;     // Number of Additional resource records.
  data_buffer[11] = 0;     // Number of Additional resource records.

  data_size = 12;
  buffer_pointer = 12;  // First byte of first Query/Record.
  type = 0;
  query_count = 0;
  answer_count = 0;
  ns_count = 0;
  ar_count = 0;
}

unsigned int MDns::PopulateName(char* name_buffer) {
  // TODO: This section does not match the full mDNS spec
  // as it does not re-use strings from previous queries.

  unsigned int buffer_pointer_start = buffer_pointer;
  int word_start = 0, word_end = 0;
  do {
    if (name_buffer[word_end] == '.' or name_buffer[word_end] == '\0') {
      int word_length = word_end - word_start;
      data_buffer[buffer_pointer++] = (unsigned byte)word_length;
      for (int i = word_start; i < word_end; ++i) {
        data_buffer[buffer_pointer++] = name_buffer[i];
      }
      word_end++;  // Skip the '.' charicter.
      word_start = word_end;
    }
    word_end++;
  } while (name_buffer[word_start] != '\0');
  data_buffer[buffer_pointer++] = '\0';  // End of qname.

  return buffer_pointer - buffer_pointer_start;
}

void MDns::AddQuery(Query query) {
  if (answer_count || ns_count || ar_count) {
    Serial.println(" ERROR. Resource records inclued before Queries.");
    return;
  }
  data_buffer[2] = 0;     // 0b00000000 for Query, 0b10000000 for Answer.
  type = 1;
  ++query_count;
  data_buffer[4] = (query_count & 0xFF00) >> 8;
  data_buffer[5] = query_count & 0xFF;

  // Create DNS name buffer from qname.
  PopulateName(query.qname_buffer);

  // The rest of the flags.
  data_buffer[buffer_pointer++] = (query.qtype & 0xFF00) >> 8;
  data_buffer[buffer_pointer++] = query.qtype & 0xFF;
  unsigned int qclass = 0;
  if (query.unicast_response) {
    qclass = 0b1000000000000000;
  }
  qclass += query.qclass;
  data_buffer[buffer_pointer++] = (qclass & 0xFF00) >> 8;
  data_buffer[buffer_pointer++] = qclass & 0xFF;
  data_size = buffer_pointer;
}

void MDns::AddAnswer(Answer answer) {
  if (ns_count || ar_count) {
    Serial.println(" ERROR. NS or AR records added before Answer records");
    return;
  }
  answer_count++;
  data_buffer[6] = (answer_count & 0xFF00) >> 8;
  data_buffer[7] = answer_count & 0xFF;

  // Create DNS name buffer from name.
  PopulateName(answer.name_buffer);

  data_buffer[buffer_pointer++] = (answer.rrtype & 0xFF00) >> 8;
  data_buffer[buffer_pointer++] = answer.rrtype & 0xFF;

  unsigned int rrclass = 0;
  if (answer.rrset) {
    rrclass = 0b1000000000000000;
  }
  rrclass += answer.rrclass;
  data_buffer[buffer_pointer++] = (rrclass & 0xFF00) >> 8;
  data_buffer[buffer_pointer++] = rrclass & 0xFF;

  data_buffer[buffer_pointer++] = (answer.rrttl & 0xFF000000) >> 24;
  data_buffer[buffer_pointer++] = (answer.rrttl & 0xFF0000) >> 16;
  data_buffer[buffer_pointer++] = (answer.rrttl & 0xFF00) >> 8;
  data_buffer[buffer_pointer++] = (answer.rrttl & 0xFF);

  unsigned int rdata_len_p0 = buffer_pointer++;
  unsigned int rdata_len_p1 = buffer_pointer++;
  unsigned int rdata_len;

  switch (answer.rrtype) {
    case MDNS_TYPE_A:  // Returns a 32-bit IPv4 address
      rdata_len = 4;
      data_buffer[buffer_pointer++] = answer.rdata_buffer[0];
      data_buffer[buffer_pointer++] = answer.rdata_buffer[1];
      data_buffer[buffer_pointer++] = answer.rdata_buffer[2];
      data_buffer[buffer_pointer++] = answer.rdata_buffer[3];
      break;
    case MDNS_TYPE_PTR:  // Pointer to a canonical name.
      rdata_len = PopulateName(answer.rdata_buffer);
      break;
    default:
      // TODO: Other record types.
      Serial.println(" **ERROR** Sending this record type not implemented yet.");
  }

  data_buffer[rdata_len_p0] = (rdata_len & 0xFF00) >> 8;
  data_buffer[rdata_len_p1] = rdata_len & 0xFF;

  data_size = buffer_pointer;
}

void MDns::Send() {
  Serial.println("Sending UDP multicast packet");

  Udp.begin(MDNS_SOURCE_PORT);
  Udp.beginPacketMulticast(IPAddress(224, 0, 0, 251), MDNS_TARGET_PORT, WiFi.localIP(), MDNS_TTL);
  Udp.write(data_buffer, data_size);
  Udp.endPacket();
}

void MDns::Display() {
  Serial.println();
  Serial.print("Packet size: ");
  Serial.print(data_size);
  Serial.print("  ");
  Serial.println(data_size, HEX);
  Serial.print(" TYPE: ");
  Serial.print(type);
  Serial.print("      QUERY_COUNT: ");
  Serial.print(query_count);
  Serial.print("      ANSWER_COUNT: ");
  Serial.print(answer_count);
  Serial.print("      NS_COUNT: ");
  Serial.print(ns_count);
  Serial.print("      AR_COUNT: ");
  Serial.println(ar_count);
}

struct Query MDns::Parse_Query() {
  struct Query return_value;
  return_value.buffer_pointer = buffer_pointer;

  buffer_pointer = nameFromDnsPointer(return_value.qname_buffer, 0, MAX_MDNS_NAME_LEN, data_buffer, buffer_pointer);

  byte qtype_0 = data_buffer[buffer_pointer++];
  byte qtype_1 = data_buffer[buffer_pointer++];
  byte qclass_0 = data_buffer[buffer_pointer++];
  byte qclass_1 = data_buffer[buffer_pointer++];

  return_value.qtype = (qtype_0 << 8) + qtype_1;

  return_value.unicast_response = (0b10000000 & qclass_0);
  return_value.qclass = ((qclass_0 & 0b01111111) << 8) + qclass_1;

  return_value.valid = true;
  if (return_value.qclass != 0xFF && return_value.qclass != 0x01) {
    // QCLASS is not ANY (0xFF) or INternet (0x01).
    Serial.print(" **ERROR QCLASS** ");
    Serial.println(return_value.qclass, HEX);
    return_value.valid = false;
  }

  if (buffer_pointer > data_size) {
    // We've over-run the returned data.
    // Something has gone wrong receiving or parseing the data.
    Serial.print(" **ERROR size** ");
    Serial.print(buffer_pointer, HEX);
    Serial.print(" ");
    Serial.println(data_size, HEX);
    return_value.valid = false;
  }
  return return_value;
}

struct Answer MDns::Parse_Answer() {
  struct Answer return_value;
  return_value.buffer_pointer = buffer_pointer;

  buffer_pointer = nameFromDnsPointer(return_value.name_buffer, 0, MAX_MDNS_NAME_LEN, data_buffer, buffer_pointer);

  return_value.rrtype = (data_buffer[buffer_pointer++] << 8) + data_buffer[buffer_pointer++];

  byte rrclass_0 = data_buffer[buffer_pointer++];
  byte rrclass_1 = data_buffer[buffer_pointer++];
  return_value.rrset = (0b10000000 & rrclass_0);
  return_value.rrclass = ((rrclass_0 & 0b01111111) << 8) + rrclass_1;

  return_value.rrttl = (data_buffer[buffer_pointer++] << 24) +
                       (data_buffer[buffer_pointer++] << 16) +
                       (data_buffer[buffer_pointer++] << 8) +
                       data_buffer[buffer_pointer++];

  PopulateAnswerResult(&return_value);

  return_value.valid = true;
  if (buffer_pointer > data_size) {
    // We've over-run the returned data.
    // Something has gone wrong receiving or parseing the data.
    Serial.print(" **ERROR size** ");
    Serial.print(buffer_pointer, HEX);
    Serial.print(" ");
    Serial.println(data_size, HEX);
    return_value.valid = false;
  }
  return return_value;
}

// Display packet contents in HEX.
void MDns::DisplayRawPacket() {
  // display the packet contents in HEX
  Serial.println("Raw packet");
  int i, j;

  for (i = 0; i <= data_size; i += 16) {
    Serial.print("0x");
    PrintHex(i >> 8); PrintHex(i);
    Serial.print("   ");
    for (j = 0; j < 16; j++) {
      if (i + j >= data_size) {
        break;
      }
      if (data_buffer[i + j] > 31 and data_buffer[i + j] < 128) {
        Serial.print((char)data_buffer[i + j]);
      } else {
        Serial.print(".");
      }
    }
    Serial.print("    ");
    for (j = 0; j < 16; j++) {
      if (i + j >= data_size) {
        break;
      }
      PrintHex(data_buffer[i + j]);
      Serial.print(' ');
    }
    Serial.println();
  }
}


void MDns::PopulateAnswerResult(Answer* answer) {
  int rdlength = (data_buffer[buffer_pointer++] << 8) + data_buffer[buffer_pointer++];

  switch (answer->rrtype) {
    case MDNS_TYPE_A:  // Returns a 32-bit IPv4 address
      if (MAX_MDNS_NAME_LEN >= 16) {
        sprintf(answer->rdata_buffer, "%u.%u.%u.%u",
                data_buffer[buffer_pointer++], data_buffer[buffer_pointer++],
                data_buffer[buffer_pointer++], data_buffer[buffer_pointer++]);
      } else {
        sprintf(answer->rdata_buffer, "ipv4");
        buffer_pointer += 4;
      }
      break;
    case MDNS_TYPE_PTR:  // Pointer to a canonical name.
      buffer_pointer = nameFromDnsPointer(answer->rdata_buffer, 0, MAX_MDNS_NAME_LEN,
                                          data_buffer, buffer_pointer);
      break;
    case MDNS_TYPE_HINFO:  // HINFO. host information
      buffer_pointer = parseText(answer->rdata_buffer, MAX_MDNS_NAME_LEN, rdlength,
                                 data_buffer, buffer_pointer);
      break;
    case MDNS_TYPE_TXT:  // Originally for arbitrary human-readable text in a DNS record.
      // We only return the first MAX_MDNS_NAME_LEN bytes of thir record type.
      buffer_pointer = parseText(answer->rdata_buffer, MAX_MDNS_NAME_LEN, rdlength,
                                 data_buffer, buffer_pointer);
      break;
    case MDNS_TYPE_AAAA:  // Returns a 128-bit IPv6 address.
      {
        int buffer_pos = 0;
        for (int i = 0; i < rdlength; i++) {
          if (buffer_pos < MAX_MDNS_NAME_LEN - 3) {
            sprintf(answer->rdata_buffer + buffer_pos, "%02X:", data_buffer[buffer_pointer++]);
          } else {
            buffer_pointer++;
          }
          buffer_pos += 3;
        }
        answer->rdata_buffer[--buffer_pos] = '\0';  // Remove trailing ':'
      }
      break;
    case MDNS_TYPE_SRV:  // Server Selection.
      {
        unsigned int priority = (data_buffer[buffer_pointer++] << 8) + data_buffer[buffer_pointer++];
        unsigned int weight = (data_buffer[buffer_pointer++] << 8) + data_buffer[buffer_pointer++];
        unsigned int port = (data_buffer[buffer_pointer++] << 8) + data_buffer[buffer_pointer++];
        sprintf(answer->rdata_buffer, "p=%u;w=%u;port=%u;target=", priority, weight, port);

        buffer_pointer = nameFromDnsPointer(answer->rdata_buffer, strlen(answer->rdata_buffer) +1, 
            MAX_MDNS_NAME_LEN - strlen(answer->rdata_buffer) -1, data_buffer, buffer_pointer);
      }
      break;
    default:
      {
        int buffer_pos = 0;
        for (int i = 0; i < rdlength; i++) {
          if (buffer_pos < MAX_MDNS_NAME_LEN - 3) {
            sprintf(answer->rdata_buffer + buffer_pos, "%02X ", data_buffer[buffer_pointer++]);
          } else {
            buffer_pointer++;
          }
          buffer_pos += 3;
        }
      }
      break;
  }
}

bool writeToBuffer(byte value, char* p_name_buffer, int* p_name_buffer_pos, int name_buffer_len) {
  if (*p_name_buffer_pos < name_buffer_len - 1) {
    *(p_name_buffer + *p_name_buffer_pos) = value;
    (*p_name_buffer_pos)++;
    *(p_name_buffer + *p_name_buffer_pos) = '\0';
    return true;
  }
  (*p_name_buffer_pos)++;
  return false;
}

int parseText(char* data_buffer, int data_buffer_len, int data_len,
              byte* p_packet_buffer, int packet_buffer_pos) {
  int i, data_buffer_pos = 0;
  for (i = 0; i < data_len; i++) {
    writeToBuffer(p_packet_buffer[packet_buffer_pos++], data_buffer, &data_buffer_pos, data_buffer_len);
  }
  data_buffer[data_buffer_pos] = '\0';
  return packet_buffer_pos;
}

int nameFromDnsPointer(char* p_name_buffer, int name_buffer_pos, int name_buffer_len,
                       byte* p_packet_buffer, int packet_buffer_pos) {

  if ((name_buffer_pos > 0) ) {
    // Since we are adding more to an already populated buffer,
    // replace the trailing EOL with the FQDN seperator.
    name_buffer_pos--;
    writeToBuffer('.', p_name_buffer, &name_buffer_pos, name_buffer_len);
  }

  if (*(p_packet_buffer + packet_buffer_pos) < 0xC0) {
    // Since the first 2 bits are not set,
    // this is the start of a name section.
    // http://www.tcpipguide.com/free/t_DNSNameNotationandMessageCompressionTechnique.htm

    int word_len = *(p_packet_buffer + packet_buffer_pos++);
    for (int l = 0; l < word_len; l++) {
      writeToBuffer(*(p_packet_buffer + packet_buffer_pos++), p_name_buffer, &name_buffer_pos, name_buffer_len);
    }

    writeToBuffer('\0', p_name_buffer, &name_buffer_pos, name_buffer_len);

    if (*(p_packet_buffer + packet_buffer_pos) > 0) {
      // Next word.
      packet_buffer_pos =
        nameFromDnsPointer(p_name_buffer, name_buffer_pos, name_buffer_len, p_packet_buffer, packet_buffer_pos);
    } else {
      // End of string.
      packet_buffer_pos++;
    }
  } else {
    // Message Compression used. Next 2 bytes are a pointer to the actual name section.
    int pointer = (*(p_packet_buffer + packet_buffer_pos++) - 0xC0) << 8;
    pointer += *(p_packet_buffer + packet_buffer_pos++);
    nameFromDnsPointer(p_name_buffer, name_buffer_pos, name_buffer_len, p_packet_buffer, pointer);
  }
  return packet_buffer_pos;
}

void Query::Display() {
  Serial.print("question  0x");
  Serial.println(buffer_pointer, HEX);
  if (!valid) {
    Serial.println(" **ERROR**");
  }
  Serial.print(" QNAME:    ");
  Serial.println(qname_buffer);
  Serial.print(" QTYPE:  0x");
  Serial.print(qtype, HEX);
  Serial.print("      QCLASS: 0x");
  Serial.print(qclass, HEX);
  Serial.print("      Unicast Response: ");
  Serial.println(unicast_response);
}

void Answer::Display() {
  Serial.print("answer  0x");
  Serial.println(buffer_pointer, HEX);
  if (!valid) {
    Serial.println(" **ERROR**");
  }
  Serial.print(" RRNAME:    ");
  Serial.println(name_buffer);
  Serial.print(" RRTYPE:  0x");
  Serial.print(rrtype, HEX);
  Serial.print("      RRCLASS: 0x");
  Serial.print(rrclass, HEX);
  Serial.print("      RRTTL: ");
  Serial.print(rrttl);
  Serial.print("      RRSET: ");
  Serial.println(rrset);
  Serial.print(" RRDATA:    ");
  Serial.println(rdata_buffer);
}

} // namespace mdns
