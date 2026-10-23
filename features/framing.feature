Feature: Complete event framing
  Consumers receive whole events even when transport reads divide documents.

  Scenario: Preserve XML extensions across arbitrary stream reads
    Given a complete XML event with unknown namespaced fields and CDATA
    When the XML stream is received one byte at a time
    Then one event contains the exact original XML bytes

  Scenario: Separate coalesced XML documents
    Given two complete XML events with different identifiers
    When both documents arrive in one stream read
    Then both events are emitted separately in their original order

  Scenario: Reject a truncated document at end of stream
    Given a complete XML event with unknown namespaced fields and CDATA
    When the stream ends before the final XML byte
    Then no partial event is emitted and truncation is reported

  Scenario Outline: Reject invalid UDP records without cross-packet assembly
    Given a UDP datagram containing <content>
    When the complete datagram is decoded
    Then the datagram is rejected without an event

    Examples:
      | content                    |
      | two XML documents          |
      | an unknown TAK version     |
      | a malformed TAK header     |
      | an incomplete XML document |
      | a receive truncation flag  |

  Scenario: Distinguish mesh version from stream payload length
    Given a TAK v1 mesh datagram carrying an event
    When the complete datagram is decoded
    Then version 1 and the exact unframed Protobuf envelope are returned
