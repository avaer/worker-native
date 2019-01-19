{
  'targets': [
    {
      'target_name': 'worker_native',
      'sources': [
        'src/main.cpp',
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "src"
      ],
      'conditions': [
        ['"<!(echo $LUMIN)"=="1"', {
          'defines': ['LUMIN'],
        }],
      ],
    },
    {
      'target_name': 'worker_native2',
      'sources': [
        'src/child.cpp',
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "src"
      ],
      'conditions': [
        ['"<!(echo $LUMIN)"=="1"', {
          'defines': ['LUMIN'],
        }],
      ],
    },
  ],
}
