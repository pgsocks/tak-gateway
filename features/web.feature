Feature: Manage live gateway endpoints through the local web interface
  Management operations use protected revisions and preserve durable settings.

  Scenario: Create endpoints through HTTP and forward a complete event
    Given a local management server with an empty protected state store
    When HTTP requests create a UDP input and TCP output
    Then a downstream consumer receives an exact event from that UDP input

  Scenario: Reject a stale endpoint edit
    Given a local management server with an empty protected state store
    When HTTP requests create a UDP input and TCP output
    And another browser submits an output change using the initial revision
    Then the stale change is rejected and the committed revision is retained

  Scenario: Preview an ATAK package before adding its input
    Given a local management server with an empty protected state store
    When an ATAK package is uploaded over HTTP for preview
    Then its input is shown without credentials and is not yet configured
